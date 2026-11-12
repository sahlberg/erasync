/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "era.h"

#include "common.h"
#include "dm.h"
#include "record.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dm-ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 512ULL
#define STATE_DIR "/var/lib/erasync"
#define LOG_DIR STATE_DIR "/era"
#define LOCK_TIMEOUT 600
#define SNAP_WAIT 10
#define MTIME_MARGIN 1.0
#define MAX_DEVS 64
#define MAX_ERAS 64
#define MAX_BITMAPS 32
#define MAX_DEPTH 16
#define FIEMAP_EXTENTS 256

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static pid_t child_pid;


/*
 * Lifetime of user.erasync.era, stored only on the destination file:
 *
 *   <era> <time> <device-id> [<era> <time> <device-id>]
 *
 * First record: source era the bytes came from. Second, when present:
 * destination era after which a non-erasync write changed the copy.
 * A record is stored only when that side is on dm-era. Taking the
 * metadata snapshot advances that dm-era device to the next era.
 *
 * One run, in order. A never-synced file starts with no xattr.
 *
 * 1. Scan destination files for local changes.
 *    Files without a destination record are ignored. These are either new
 *    files never synced or files that were aborted during a previous sync.
 *    For every other file, note the regions changed by something other than
 *    erasync since that era. The destination era in the xattr is used to
 *    determine these regions.
 *    The first file with a destination record advances the destination dm-era
 *    device to the next era, even when that file has no local changes.
 *
 * 2. Check the source files for changes since last sync.
 *    A file is not checked when step 1 noted nothing and the size and
 *    modification time already match. For every other file, note the
 *    regions changed on the source. The source era in the xattr is used
 *    to determine these regions.
 *    The first file that is checked advances the source dm-era device to
 *    the next era, even when that file has no changed regions.
 *
 * 3. Update the files.
 *    A file noted in step 1 is left unchanged when the source is unchanged
 *    and the size matches. A metadata-only file has its mode, owner, and
 *    modification time applied.
 *    A content update, in order:
 *    .1 Truncate the file to the source size.
 *    .2 Copy each region from steps 1 and 2. Write the data, or punch a
 *        hole when the source region is a hole.
 *    .3 Truncate to the source size again.
 *    .4 Apply the mode, owner, and modification time.
 *    .5 Sync the bytes.
 *    The xattr is not changed here. A first copy writes its source record
 *    onto the temporary file before the rename, as step 4 describes.
 *
 * 4. A copied file keeps its old xattr until its bytes are synced, then
 *    stores the new source record and keeps the previous destination record.
 *    A first copy sets that record on a temporary file and renames it into
 *    place. A skip or a metadata update is queued only when a destination
 *    record is missing.
 *
 * 5. Update the destination record.
 *    Replace it on every copied file, and store it on every skipped or
 *    metadata-only file that had no destination record. A file that
 *    already has one and was not copied is left unchanged. The first
 *    such file on a destination device advances that dm-era device to
 *    the next era, and that new era is the record written to those
 *    files.
 *
 * A file with no era is treated as a file where every byte has changed.
 */


static double era_now(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * A write that lands close to the snapshot may not be in the bitmap yet,
 * so only trust an mtime that is clearly older than the snapshot.
 */
static int mtime_is_ok(const struct stat *st, double snap_time)
{
        double mtime = st->st_mtim.tv_sec + st->st_mtim.tv_nsec / 1e9;

        return mtime < snap_time - MTIME_MARGIN;
}


/*
 * The blocks of an era device that were written in or after since_era,
 * as era_invalidate reported them from the metadata snapshot taken at
 * snap_time. That snapshot started the era in new_stamp.
 *
 * Files on the same device can have different reference eras, so one
 * device can hold several bitmaps. Each is read once per run and shared
 * by every file with that reference era.
 */
struct era_bitmap {
        uint64_t since_era;
        double snap_time;
        char new_stamp[80];
        uint64_t *bits;
};

/*
 * A dm-era device and its log, /var/lib/erasync/era/<id>.
 *
 * Every snapshot appends its "<era> <time>" stamp to the log. An era
 * number alone does not identify an era, since recreating the metadata
 * restarts the counter, so an era from an xattr is only trusted when its
 * exact stamp is in the log. The log is also flocked while a snapshot is
 * held, because dm-era allows only one snapshot at a time.
 */
struct era_dev {
        dev_t dev;
        dev_t meta_dev;
        uint64_t block_sectors;
        uint64_t nr_blocks;
        char id[DM_UUID_LEN + DM_NAME_LEN + 8];
        char log_path[4096];
        int log_fd;
        char last_stamp[80];
        double last_snap_time;
        /* era_finish has advanced this destination device. */
        int dest_era_advanced;
        int n_bitmaps;
        struct era_bitmap bitmaps[MAX_BITMAPS];
};

static struct era_dev *eras[MAX_ERAS];
static int n_eras;
static struct era_dev *snap_era;

static uint64_t bitmap_words(const struct era_dev *e)
{
        return (e->nr_blocks + 63) / 64;
}

static void bitmap_set(struct era_bitmap *bm, uint64_t block)
{
        bm->bits[block / 64] |= 1ULL << (block % 64);
}

/* Blocks past the end of the device are always dirty. */
static int block_is_dirty(const struct era_dev *e,
                          const struct era_bitmap *bm, uint64_t block)
{
        if (block >= e->nr_blocks) {
                return 1;
        }
        return (bm->bits[block / 64] >> (block % 64)) & 1;
}

static int bitmap_empty(const struct era_dev *e, const struct era_bitmap *bm)
{
        uint64_t i;

        for (i = 0; i < bitmap_words(e); i++) {
                if (bm->bits[i]) {
                        return 0;
                }
        }
        return 1;
}

/* The era device dev, which has the table tab and the era target params. */
static struct era_dev *get_era(dev_t dev, struct dm_table *tab,
                               const char *params)
{
        unsigned int meta_major;
        unsigned int meta_minor;
        unsigned long long block_sectors;
        uint64_t sectors = 0;
        struct era_dev *e;
        char *p;
        int i;

        for (i = 0; i < n_eras; i++) {
                if (eras[i]->dev == dev) {
                        return eras[i];
                }
        }
        if (n_eras == MAX_ERAS) {
                return NULL;
        }
        if (sscanf(params, "%u:%u %*u:%*u %llu",
                   &meta_major, &meta_minor, &block_sectors) != 3) {
                return NULL;
        }
        if (block_sectors == 0) {
                return NULL;
        }

        for (i = 0; i < tab->n_targets; i++) {
                sectors += tab->targets[i].len;
        }

        e = xmalloc(sizeof *e);
        e->dev = dev;
        e->meta_dev = makedev(meta_major, meta_minor);
        e->block_sectors = block_sectors;
        e->nr_blocks = (sectors + block_sectors - 1) / block_sectors;
        e->log_fd = -1;
        snprintf(e->id, sizeof e->id, "%s", tab->id);
        for (p = e->id; *p; p++) {
                if (*p == '/' || isspace((unsigned char)*p)) {
                        *p = '_';
                }
        }
        snprintf(e->log_path, sizeof e->log_path, "%s/%s", LOG_DIR, e->id);
        eras[n_eras++] = e;
        return e;
}

/*
 * State for mapping a file extent down the device stack to its era device.
 * When dirty is set, the file ranges whose blocks are set in bitmap are
 * added to it, clipped to extent_end.
 */
struct walk {
        struct era_dev *era;
        const char *err;
        struct era_ranges *dirty;
        struct era_bitmap *bitmap;
        off_t extent_end;
};

static int walk_error(struct walk *w, const char *err)
{
        w->err = err;
        return -1;
}

/*
 * Add the file ranges of the era device sectors [sector, sector + nsect)
 * whose blocks are dirty. offset is the file offset of sector.
 */
static void add_dirty_blocks(struct walk *w, uint64_t sector, uint64_t nsect,
                             off_t offset)
{
        uint64_t block_sectors = w->era->block_sectors;
        uint64_t end = sector + nsect;
        uint64_t s = sector;

        while (s < end) {
                uint64_t block = s / block_sectors;
                uint64_t block_end = MIN((block + 1) * block_sectors, end);
                off_t start = offset + (off_t)(s - sector) * (off_t)SECTOR;
                off_t len = (off_t)(block_end - s) * (off_t)SECTOR;

                if (block_is_dirty(w->era, w->bitmap, block) &&
                    start < w->extent_end) {
                        era_add_range(w->dirty, start,
                                      MIN(len, w->extent_end - start));
                }
                s = block_end;
        }
}


static int map_range(struct walk *w, dev_t dev, uint64_t sector,
                     uint64_t nsect, off_t offset, int depth);

/* dev is not a device-mapper device. Map a partition onto its disk. */
static int map_partition(struct walk *w, dev_t dev, uint64_t sector,
                         uint64_t nsect, off_t offset, int depth)
{
        char buf[64];
        uint64_t start;
        unsigned int ma;
        unsigned int mi;

        if (!sysfs_read(dev, "partition", buf, sizeof buf)) {
                return walk_error(w, "not on a dm-era device");
        }
        if (!sysfs_read(dev, "start", buf, sizeof buf)) {
                return walk_error(w, "not on a dm-era device");
        }
        start = strtoull(buf, NULL, 10);
        if (!sysfs_read(dev, "../dev", buf, sizeof buf)) {
                return walk_error(w, "not on a dm-era device");
        }
        if (sscanf(buf, "%u:%u", &ma, &mi) != 2) {
                return walk_error(w, "not on a dm-era device");
        }
        return map_range(w, makedev(ma, mi), sector + start, nsect, offset,
                         depth + 1);
}

/*
 * Map the sectors [sector, sector + nsect) of dev down through partitions
 * and linear and crypt targets until they reach a dm-era target. offset is
 * the file offset of sector. Sets w->era, and adds dirty blocks when
 * w->dirty is set.
 */
static int map_range(struct walk *w, dev_t dev, uint64_t sector,
                     uint64_t nsect, off_t offset, int depth)
{
        struct dm_table *tab;
        char buf[64];

        if (depth > MAX_DEPTH) {
                return walk_error(w, "device stack too deep");
        }
        if (!sysfs_read(dev, "dm/name", buf, sizeof buf)) {
                return map_partition(w, dev, sector, nsect, offset, depth);
        }
        tab = dm_get_table(dev);
        if (!tab) {
                return walk_error(w, "cannot read device-mapper table");
        }

        while (nsect) {
                struct dm_tgt *t;
                uint64_t target_offset;
                uint64_t n;
                dev_t lower_dev;
                uint64_t lower_sector;

                t = dm_find_target(tab, sector);
                if (!t) {
                        return walk_error(w,
                                          "sector outside device-mapper table");
                }
                target_offset = sector - t->start;
                n = MIN(t->len - target_offset, nsect);

                if (strcmp(t->type, "era") == 0) {
                        struct era_dev *e = get_era(dev, tab, t->params);

                        if (!e) {
                                return walk_error(w, "cannot parse era table");
                        }
                        if (w->era && w->era != e) {
                                return walk_error(w, "file spans more than "
                                                  "one dm-era device");
                        }
                        w->era = e;
                        if (w->dirty) {
                                add_dirty_blocks(w, sector, n, offset);
                        }
                } else if (dm_lower_device(t, &lower_dev, &lower_sector)) {
                        if (map_range(w, lower_dev,
                                      lower_sector + target_offset, n,
                                      offset, depth + 1) < 0) {
                                return -1;
                        }
                } else {
                        return walk_error(w, "unsupported device-mapper "
                                          "target");
                }

                sector += n;
                nsect -= n;
                offset += (off_t)(n * SECTOR);
        }
        return 0;
}

/*
 * The dm-era device below the filesystem on dev. Returns NULL when there
 * is none and sets *why, unless why is NULL, to the reason.
 */
static struct era_dev *era_for_dev(dev_t dev, const char **why)
{
        static struct {
                dev_t dev;
                struct era_dev *era;
                const char *why;
        } cache[MAX_DEVS];
        static int n_cache;
        struct walk w;
        int i;

        for (i = 0; i < n_cache; i++) {
                if (cache[i].dev == dev) {
                        if (why) {
                                *why = cache[i].why;
                        }
                        return cache[i].era;
                }
        }

        memset(&w, 0, sizeof w);
        if (major(dev) == 0) {
                w.err = "filesystem has no single backing block device";
        } else {
                map_range(&w, dev, 0, 1, 0, 0);
        }
        if (w.err) {
                w.era = NULL;
        }

        if (n_cache < MAX_DEVS) {
                cache[n_cache].dev = dev;
                cache[n_cache].era = w.era;
                cache[n_cache].why = w.err;
                n_cache++;
        }
        if (why) {
                *why = w.err;
        }
        return w.era;
}

const char *era_device_problem(dev_t dev)
{
        static const char plain[] = "not on a dm-era device";
        static char msg[160];
        const char *why;

        if (era_for_dev(dev, &why)) {
                return NULL;
        }
        if (!why || strcmp(why, plain) == 0) {
                return plain;
        }
        snprintf(msg, sizeof msg, "%s (%s)", plain, why);
        return msg;
}

/*
 * The current era of e. *held, when not NULL, is set to the era of
 * the held metadata snapshot, or 0 when none is held.
 */
static uint32_t era_status(struct era_dev *e, uint64_t *held)
{
        struct dm_table *tab;
        unsigned int current;
        char snap[32];

        tab = dm_read_table(e->dev, 0);
        if (!tab) {
                die("cannot read status of era device %s", e->id);
        }
        if (tab->n_targets != 1 ||
            sscanf(tab->targets[0].params, "%*u %*u/%*u %u %31s",
                   &current, snap) != 2) {
                die("cannot read status of era device %s", e->id);
        }
        dm_free_table(tab);

        if (held) {
                if (strcmp(snap, "-") == 0) {
                        *held = 0;
                } else {
                        *held = strtoull(snap, NULL, 10);
                }
        }
        return current;
}

static const char *era_invalidate_path(void)
{
        static const char *dirs[] = {
                "/usr/sbin", "/sbin", "/usr/bin", "/bin", NULL
        };
        static char path[64];
        int i;

        for (i = 0; dirs[i]; i++) {
                snprintf(path, sizeof path, "%s/era_invalidate", dirs[i]);
                if (access(path, X_OK) == 0) {
                        return path;
                }
        }
        return NULL;
}

static void era_lock(struct era_dev *e)
{
        double deadline = era_now() + LOCK_TIMEOUT;

        if (mkdir(STATE_DIR, 0755) < 0 && errno != EEXIST) {
                die("cannot create %s: %s", STATE_DIR, strerror(errno));
        }
        if (mkdir(LOG_DIR, 0700) < 0 && errno != EEXIST) {
                die("cannot create %s: %s", LOG_DIR, strerror(errno));
        }
        e->log_fd = open(e->log_path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC,
                         0600);
        if (e->log_fd < 0) {
                die("cannot open %s: %s", e->log_path, strerror(errno));
        }

        while (flock(e->log_fd, LOCK_EX | LOCK_NB) < 0) {
                if (errno != EWOULDBLOCK) {
                        die("flock %s: %s", e->log_path,
                                  strerror(errno));
                }
                if (erasync_stop) {
                        die("interrupted");
                }
                if (era_now() > deadline) {
                        die("timed out waiting for another run on era "
                                  "device %s", e->id);
                }
                if (opt_verbose) {
                        fprintf(stderr, "erasync: waiting for another run "
                                "on %s\n", e->id);
                }
                sleep(1);
        }
}

static void era_unlock(struct era_dev *e)
{
        if (e->log_fd < 0) {
                return;
        }
        close(e->log_fd);
        e->log_fd = -1;
}

/* Whether the log of e has the line stamp. */
static int log_has_stamp(struct era_dev *e, const char *stamp)
{
        uint32_t current = era_status(e, NULL);
        char line[128];
        int found = 0;
        FILE *f;

        f = fopen(e->log_path, "r");
        if (!f) {
                return 0;
        }
        while (fgets(line, sizeof line, f)) {
                char *nl = strchr(line, '\n');
                unsigned long long era;
                char *end;

                if (!nl) {
                        die("%s has a line that is too long",
                                  e->log_path);
                }
                *nl = '\0';
                era = strtoull(line, &end, 10);
                if (end == line || *end != ' ') {
                        continue;
                }
                if (era > current) {
                        die("%s mentions era %llu but %s is at era %u: "
                                  "the era metadata was recreated; remove "
                                  "the log", e->log_path, era, e->id, current);
                }
                if (strcmp(line, stamp) == 0) {
                        found = 1;
                }
        }
        fclose(f);
        return found;
}

/* Stop era_invalidate and drop the metadata snapshot, if either is held. */
static void snap_drop(void)
{
        if (child_pid > 0) {
                kill(child_pid, SIGKILL);
                waitpid(child_pid, NULL, 0);
                child_pid = 0;
        }
        if (!snap_era) {
                return;
        }
        if (dm_message(snap_era->dev, "drop_metadata_snap") < 0) {
                fprintf(stderr, "erasync: drop_metadata_snap on %s: %s\n",
                        snap_era->id, strerror(errno));
        }
        snap_era = NULL;
}

/*
 * Take the metadata snapshot, which also advances e to the next era.
 * Waits a while if someone else holds a snapshot.
 */
static void snap_take(struct era_dev *e)
{
        double deadline = era_now() + SNAP_WAIT;
        uint64_t held;

        for (;;) {
                if (erasync_stop) {
                        die("interrupted");
                }
                era_status(e, &held);
                if (!held) {
                        if (dm_message(e->dev, "take_metadata_snap") == 0) {
                                break;
                        }
                        if (errno != EINVAL) {
                                die("take_metadata_snap on %s: %s",
                                          e->id, strerror(errno));
                        }
                }
                if (era_now() > deadline) {
                        die("era device %s has a metadata snapshot "
                                  "held that is not ours. If it is stale, "
                                  "release it with 'dmsetup message "
                                  "<device> 0 drop_metadata_snap'", e->id);
                }
                usleep(200000);
        }
        snap_era = e;
}

/* Run era_invalidate on the snapshot of e and return its output. */
static FILE *start_era_invalidate(struct era_dev *e, uint64_t since)
{
        const char *tool = era_invalidate_path();
        char meta_path[64];
        char since_str[32];
        int pfd[2];
        FILE *f;

        if (!tool) {
                die("era_invalidate not found");
        }
        snprintf(meta_path, sizeof meta_path, "/dev/block/%u:%u",
                 major(e->meta_dev), minor(e->meta_dev));
        snprintf(since_str, sizeof since_str, "%" PRIu64, since);

        if (pipe(pfd) < 0) {
                die("pipe: %s", strerror(errno));
        }
        child_pid = fork();
        if (child_pid < 0) {
                die("fork: %s", strerror(errno));
        }
        if (child_pid == 0) {
                close(pfd[0]);
                if (dup2(pfd[1], STDOUT_FILENO) < 0) {
                        _exit(127);
                }
                close(pfd[1]);
                execl(tool, tool, "--metadata-snapshot",
                      "--written-since", since_str, meta_path, (char *)NULL);
                _exit(127);
        }

        close(pfd[1]);
        f = fdopen(pfd[0], "r");
        if (!f) {
                die("fdopen: %s", strerror(errno));
        }
        return f;
}

/* Fill bm with the blocks of e written in or after the era since. */
static void read_dirty_blocks(struct era_dev *e, struct era_bitmap *bm,
                              uint64_t since)
{
        char line[256];
        int status;
        FILE *f;

        f = start_era_invalidate(e, since);
        while (fgets(line, sizeof line, f)) {
                unsigned long long begin;
                unsigned long long end;

                if (erasync_stop) {
                        die("interrupted");
                }
                if (sscanf(line, " <block block=\"%llu\"", &begin) == 1) {
                        end = begin + 1;
                } else if (sscanf(line, " <range begin=\"%llu\" end=\"%llu\"",
                                  &begin, &end) != 2) {
                        continue;
                }
                if (begin >= end || end > e->nr_blocks) {
                        die("era_invalidate reported bad block range "
                                  "%llu-%llu", begin, end);
                }
                for (; begin < end; begin++) {
                        bitmap_set(bm, begin);
                }
        }
        if (ferror(f) && erasync_stop) {
                die("interrupted");
        }
        fclose(f);

        if (waitpid(child_pid, &status, 0) != child_pid) {
                status = -1;
        }
        child_pid = 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                die("era_invalidate failed on %s (status %d)",
                          e->id, status);
        }
}

static const char *era_ctx;
static unsigned long long era_ctx_size;

void era_set_context(const char *rel, unsigned long long file_size)
{
        era_ctx = rel && rel[0] ? rel : NULL;
        era_ctx_size = file_size;
}

/* Tell the user which file caused the next snapshot. */
static void print_context(struct era_dev *e)
{
        char size[32];

        if (opt_quiet || !era_ctx) {
                return;
        }
        if (era_ctx_size == 0) {
                fprintf(stderr, "erasync: %s (%s)\n", era_ctx, e->id);
                return;
        }
        format_bytes(size, sizeof size, era_ctx_size);
        fprintf(stderr, "erasync: %s, file %s (%s)\n", era_ctx, size, e->id);
}

static void log_append(struct era_dev *e, const char *stamp)
{
        char line[96];
        int len;

        len = snprintf(line, sizeof line, "%s\n", stamp);
        if (len < 0 || len >= (int)sizeof line) {
                die("era stamp is too long");
        }
        if (write(e->log_fd, line, len) != len) {
                die("cannot write %s: %s", e->log_path,
                          strerror(errno));
        }
        if (fsync(e->log_fd) < 0) {
                die("cannot write %s: %s", e->log_path,
                          strerror(errno));
        }
}

/*
 * Advance e to the next era: syncfs, take the metadata snapshot and log
 * the new era. When since is not 0, also read the blocks written in or
 * after that era into a new bitmap and return it. Returns NULL when since
 * is 0 or when there is no room for another bitmap.
 */
static struct era_bitmap *era_advance(struct era_dev *e, int fd,
                                      uint64_t since)
{
        struct era_bitmap *bm = NULL;
        double snap_time;
        uint32_t era;
        char stamp[80];
        int n;

        if (erasync_stop) {
                die("interrupted");
        }
        print_context(e);
        if (since) {
                if (e->n_bitmaps == MAX_BITMAPS) {
                        return NULL;
                }
                bm = &e->bitmaps[e->n_bitmaps];
        }

        era_lock(e);
        snap_time = era_now();
        if (syncfs(fd) < 0) {
                die("syncfs: %s", strerror(errno));
        }
        snap_take(e);
        era = era_status(e, NULL);
        n = snprintf(stamp, sizeof stamp, "%u %.9f", era, era_now());
        if (n < 0 || n >= (int)sizeof stamp) {
                die("era stamp is too long");
        }
        log_append(e, stamp);
        snprintf(e->last_stamp, sizeof e->last_stamp, "%s", stamp);
        e->last_snap_time = snap_time;

        if (bm) {
                bm->since_era = since;
                bm->snap_time = snap_time;
                snprintf(bm->new_stamp, sizeof bm->new_stamp, "%s", stamp);
                bm->bits = xmalloc(bitmap_words(e) * sizeof *bm->bits);
                read_dirty_blocks(e, bm, since);
                e->n_bitmaps++;
        }
        snap_drop();
        era_unlock(e);

        if (opt_verbose) {
                fprintf(stderr, "erasync: %s: snapshot, now era %u%s\n",
                        e->id, era, bm ? ", read dirty blocks" : "");
        }
        return bm;
}

/*
 * The bitmap of blocks written since the era since. When every bitmap
 * slot is used, fall back to the bitmap with the closest older era. It
 * covers more blocks than needed, which only costs extra copying.
 */
static struct era_bitmap *get_bitmap(struct era_dev *e, int fd,
                                     uint64_t since)
{
        struct era_bitmap *best = NULL;
        struct era_bitmap *bm;
        int i;

        for (i = 0; i < e->n_bitmaps; i++) {
                bm = &e->bitmaps[i];
                if (bm->since_era == since) {
                        return bm;
                }
                if (bm->since_era > since) {
                        continue;
                }
                if (!best || bm->since_era > best->since_era) {
                        best = bm;
                }
        }
        bm = era_advance(e, fd, since);
        if (bm) {
                return bm;
        }
        return best;
}

/*
 * Extents we cannot reason about, or that may share blocks (reflink).
 * Unwritten extents are zeros: the source queues them like holes so the
 * destination punches, and the bytes are not read.
 */
#define ALWAYS_DIRTY (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |      \
                      FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED | \
                      FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE | \
                      FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_SHARED)

/*
 * Add the ranges of fd that are dirty in bm. With include_holes, holes and
 * unwritten extents are added too, so the destination punches them.
 * Returns NULL, or the reason the ranges could not be worked out.
 */
static const char *extents_to_dirty(int fd, off_t size, dev_t dev,
                                    struct era_dev *e, struct era_bitmap *bm,
                                    struct era_ranges *dirty,
                                    int include_holes)
{
        struct fiemap *fm;
        struct walk w;
        const char *err = NULL;
        off_t pos = 0;
        int last = 0;

        memset(&w, 0, sizeof w);
        w.era = e;
        w.dirty = dirty;
        w.bitmap = bm;

        fm = xmalloc(sizeof *fm +
                     FIEMAP_EXTENTS * sizeof (struct fiemap_extent));
        while (!last && pos < size) {
                unsigned int i;

                if (erasync_stop) {
                        die("interrupted");
                }
                memset(fm, 0, sizeof *fm);
                fm->fm_start = pos;
                fm->fm_length = size - pos;
                fm->fm_flags = FIEMAP_FLAG_SYNC;
                fm->fm_extent_count = FIEMAP_EXTENTS;
                if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
                        err = "FIEMAP not supported";
                        goto out;
                }
                if (fm->fm_mapped_extents == 0) {
                        break;
                }

                for (i = 0; i < fm->fm_mapped_extents; i++) {
                        struct fiemap_extent *fe = &fm->fm_extents[i];
                        off_t start = MAX((off_t)fe->fe_logical, pos);
                        off_t end = MIN((off_t)(fe->fe_logical +
                                                fe->fe_length), size);
                        off_t phys = (off_t)fe->fe_physical +
                                (start - (off_t)fe->fe_logical);

                        last = fe->fe_flags & FIEMAP_EXTENT_LAST;
                        if (start >= end) {
                                continue;
                        }
                        if (include_holes) {
                                era_add_range(dirty, pos, start - pos);
                        }

                        if (fe->fe_flags & FIEMAP_EXTENT_UNWRITTEN) {
                                if (include_holes) {
                                        era_add_range(dirty, start,
                                                      end - start);
                                }
                        } else if ((fe->fe_flags & ALWAYS_DIRTY) ||
                                   phys % (off_t)SECTOR) {
                                era_add_range(dirty, start, end - start);
                        } else {
                                uint64_t nsect = (end - start + SECTOR - 1) /
                                        SECTOR;

                                w.extent_end = end;
                                if (map_range(&w, dev, phys / SECTOR, nsect,
                                              start, 0) < 0) {
                                        err = w.err;
                                        goto out;
                                }
                        }
                        pos = end;
                }

                if (fm->fm_mapped_extents < FIEMAP_EXTENTS) {
                        break;
                }
        }
        if (include_holes && pos < size) {
                era_add_range(dirty, pos, size - pos);
        }

out:
        free(fm);
        return err;
}


const char *era_file_ranges(int fd, const struct stat *st, const char *state,
                            struct era_ranges *dirty, char *state_out,
                            size_t state_sz, int *mtime_ok)
{
        struct era_bitmap *bm;
        struct era_dev *e;
        const char *p = state;
        const char *why;
        char stamp[80];
        char id[300];
        unsigned long long since;

        dirty->n = 0;
        state_out[0] = '\0';
        *mtime_ok = 1;
        if (!S_ISREG(st->st_mode)) {
                return "not a regular file";
        }
        e = era_for_dev(st->st_dev, &why);
        if (!e) {
                return why ? why : "not on a dm-era device";
        }
        if (record_parse(&p, &since, stamp, sizeof stamp,
                         id, sizeof id) != 1) {
                return "destination was last synced from a different era "
                        "device";
        }
        if (strcmp(id, e->id) != 0) {
                return "destination was last synced from a different era "
                        "device";
        }
        if (!log_has_stamp(e, stamp)) {
                return "destination's era is not in the era log";
        }
        bm = get_bitmap(e, fd, since);
        if (!bm) {
                return "too many different reference eras";
        }

        *mtime_ok = mtime_is_ok(st, bm->snap_time);
        why = extents_to_dirty(fd, st->st_size, st->st_dev, e, bm, dirty, 1);
        if (why) {
                return why;
        }
        if (!record_format(state_out, state_sz, bm->new_stamp, e->id)) {
                die("era state for device %s is too long", e->id);
        }
        return NULL;
}

void era_state_for_copy(int fd, const struct stat *st, char *state_out,
                        size_t state_sz, int *mtime_ok)
{
        struct era_dev *e;

        state_out[0] = '\0';
        *mtime_ok = 1;
        if (!S_ISREG(st->st_mode)) {
                return;
        }
        e = era_for_dev(st->st_dev, NULL);
        if (!e) {
                return;
        }
        if (!e->last_stamp[0]) {
                era_advance(e, fd, 0);
        }
        *mtime_ok = mtime_is_ok(st, e->last_snap_time);
        if (!record_format(state_out, state_sz, e->last_stamp, e->id)) {
                die("era state for device %s is too long", e->id);
        }
}

/* Mark the whole destination file as changed and tell the user why. */
static void dest_all_dirty(struct era_ranges *dirty, const struct stat *st,
                           const char *path, const char *why)
{
        if (opt_verbose) {
                fprintf(stderr, "erasync: %s: %s\n", path, why);
        }
        era_free_ranges(dirty);
        era_add_range(dirty, 0, st->st_size);
}

void era_dest_ranges(int fd, const struct stat *st, const char *path,
                     struct era_ranges *dirty)
{
        char state[ERA_STATE_MAX];
        char stamp[80];
        char id[300];
        struct era_dev *e;
        struct era_bitmap *bm;
        const char *why;

        dirty->n = 0;
        if (!S_ISREG(st->st_mode)) {
                return;
        }
        if (st->st_size <= 0) {
                return;
        }
        if (era_get_xattr(fd, state, sizeof state) < 0) {
                return;
        }
        if (!record_has_dest(state, stamp, sizeof stamp, id, sizeof id)) {
                return;
        }
        e = era_for_dev(st->st_dev, NULL);
        if (!e) {
                return;
        }

        if (strcmp(id, e->id) != 0) {
                dest_all_dirty(dirty, st, path,
                               "destination era device changed");
                return;
        }
        if (!log_has_stamp(e, stamp)) {
                dest_all_dirty(dirty, st, path,
                               "destination era is not in the era log");
                return;
        }
        bm = get_bitmap(e, fd, strtoull(stamp, NULL, 10));
        if (!bm) {
                dest_all_dirty(dirty, st, path, "too many destination eras");
                return;
        }
        if (bitmap_empty(e, bm)) {
                return;
        }
        why = extents_to_dirty(fd, st->st_size, st->st_dev, e, bm, dirty, 0);
        if (why) {
                dest_all_dirty(dirty, st, path, why);
                return;
        }

        if (dirty->n && opt_verbose) {
                fprintf(stderr, "erasync: %s: destination was written after "
                        "its recorded era\n", path);
        }
}

/*
 * Files that need a destination record once the sync has completed.
 *
 * era_note keeps track of a file whose content was updated.
 * state is the source record already stored on that file.
 *
 * era_arm keeps track of a file whose content was not updated.
 * The file was skipped, or only its mode, owner, and modification
 * time changed. It is tracked only when it still has no destination
 * record.
 *
 * era_finish writes the destination record on each file here.
 */
struct era_note {
        char *path;
        char *state;
        dev_t dev;
};

static struct era_note *notes;
static int n_notes;
static int alloc_notes;

static void note_add(const char *path, dev_t dev, const char *state)
{
        struct era_note *note;

        if (!state || !state[0]) {
                return;
        }
        if (n_notes == alloc_notes) {
                alloc_notes = alloc_notes ? alloc_notes * 2 : 64;
                notes = xrealloc(notes, (size_t)alloc_notes * sizeof *notes);
        }
        note = &notes[n_notes++];
        note->path = xstrdup(path);
        note->state = xstrdup(state);
        note->dev = dev;
}

/*
 * Content was updated. state is the source record stored on path.
 */
void era_note(const char *path, const struct stat *st, const char *state)
{
        note_add(path, st->st_dev, state);
}

/*
 * Content was not updated. Keep track of path when it still has no
 * destination record.
 */
void era_arm(int fd, const char *path, const struct stat *st)
{
        char state[ERA_STATE_MAX];

        if (!S_ISREG(st->st_mode)) {
                return;
        }
        if (era_get_xattr(fd, state, sizeof state) < 0) {
                return;
        }
        if (!record_source_only(state)) {
                return;
        }
        if (!era_for_dev(st->st_dev, NULL)) {
                return;
        }
        note_add(path, st->st_dev, state);
}

static int open_noted(const struct era_note *note)
{
        int fd;

        fd = open(note->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
                die("cannot open %s to record the destination era: %s",
                          note->path, strerror(errno));
        }
        return fd;
}

/* Append the destination record of e to the source record of note. */
static void write_dest_record(const struct era_note *note,
                              const struct era_dev *e)
{
        char state[ERA_STATE_MAX];
        int fd;
        int n;

        n = snprintf(state, sizeof state, "%s %s %s",
                     note->state, e->last_stamp, e->id);
        if (n < 0 || n >= (int)sizeof state) {
                die("destination era state for %s is too long",
                          note->path);
        }
        fd = open_noted(note);
        record_store(fd, note->path, state);
        if (fsync(fd) < 0) {
                die("fsync %s: %s", note->path, strerror(errno));
        }
        close(fd);
}

void era_finish(void)
{
        struct era_dev *e;
        int fd;
        int i;

        era_set_context(NULL, 0);

        /* Advance each destination device once, to the era for the records */
        for (i = 0; i < n_notes; i++) {
                e = era_for_dev(notes[i].dev, NULL);
                if (!e || e->dest_era_advanced) {
                        continue;
                }
                fd = open_noted(&notes[i]);
                era_advance(e, fd, 0);
                close(fd);
                e->dest_era_advanced = 1;
                if (opt_verbose) {
                        fprintf(stderr, "erasync: %s: destination watermark "
                                "%s\n", e->id, e->last_stamp);
                }
        }

        for (i = 0; i < n_notes; i++) {
                e = era_for_dev(notes[i].dev, NULL);
                if (!e) {
                        continue;
                }
                write_dest_record(&notes[i], e);
        }

        for (i = 0; i < n_notes; i++) {
                free(notes[i].path);
                free(notes[i].state);
        }
        free(notes);
        notes = NULL;
        n_notes = 0;
        alloc_notes = 0;
}


int era_init(const char *where)
{
        const char *why = NULL;

        if (geteuid() != 0) {
                why = "must run as root to use dm-era";
        } else if (!era_invalidate_path()) {
                why = "era_invalidate not found "
                        "(install thin-provisioning-tools)";
        } else if (dm_open() < 0) {
                why = "cannot open /dev/mapper/control";
        }
        if (why) {
                fprintf(stderr, "erasync: %s%s\n", where, why);
                return -1;
        }
        return 0;
}

void era_shutdown(void)
{
        int i;
        int j;

        snap_drop();
        for (i = 0; i < n_eras; i++) {
                era_unlock(eras[i]);
                for (j = 0; j < eras[i]->n_bitmaps; j++) {
                        free(eras[i]->bitmaps[j].bits);
                }
                free(eras[i]);
                eras[i] = NULL;
        }
        n_eras = 0;

        dm_close();
}
