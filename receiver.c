/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "receiver.h"

#include "common.h"
#include "era.h"
#include "fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void receiver_init(struct receiver *r, const char *root, int client,
                   report_fn *report, void *report_ctx)
{
        memset(r, 0, sizeof *r);
        r->root = xstrdup(root);
        r->base_fd = -1;
        r->client = client;
        r->report = report;
        r->report_ctx = report_ctx;
}

void receiver_free(struct receiver *r)
{
        free(r->root);
        free(r->base);
        free(r->levels);
        if (r->base_fd >= 0) {
                close(r->base_fd);
        }
        memset(r, 0, sizeof *r);
        r->base_fd = -1;
}

void dest_state_free(struct dest_state *s)
{
        free(s->xattr);
        era_free_ranges(&s->dirty);
        memset(s, 0, sizeof *s);
}

static void report(struct receiver *r, int action, const char *rel,
                   const char *detail)
{
        r->report(r->report_ctx, action, rel, detail);
}

static void report_error(struct receiver *r, const char *rel,
                         const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

static void report_error(struct receiver *r, const char *rel,
                         const char *fmt, ...)
{
        char msg[512];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        report(r, A_ERROR, rel, msg);
}

static struct dir_level *push_level(struct receiver *r)
{
        struct dir_level *level;

        if (r->n_levels == r->cap_levels) {
                r->cap_levels = r->cap_levels ? r->cap_levels * 2 : 16;
                r->levels = xrealloc(r->levels,
                                     r->cap_levels * sizeof *r->levels);
        }
        level = &r->levels[r->n_levels++];
        memset(level, 0, sizeof *level);
        return level;
}

/* Whether the directory that the next entry goes into has failed. */
static int parent_failed(struct receiver *r)
{
        return r->n_levels && r->levels[r->n_levels - 1].failed;
}

/* The path of rel for xattr messages and era_finish. */
static char *full_path(struct receiver *r, const char *rel)
{
        return path_join(r->base, rel);
}

/*
 * Open every directory of rel except the last component, without
 * following symlinks. *final gets the last component.
 */
static int walk_parent(int base, const char *rel, char **final)
{
        char *copy;
        char *cursor;
        char *slash;
        int fd;
        int next;

        if (bad_rel(rel)) {
                errno = EINVAL;
                return -1;
        }
        fd = dup(base);
        if (fd < 0) {
                return -1;
        }

        copy = xstrdup(rel);
        cursor = copy;
        while ((slash = strchr(cursor, '/')) != NULL) {
                *slash = '\0';
                next = openat(fd, cursor,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (next < 0) {
                        int saved_errno = errno;

                        close(fd);
                        free(copy);
                        errno = saved_errno;
                        return -1;
                }
                close(fd);
                fd = next;
                cursor = slash + 1;
        }
        *final = xstrdup(cursor);
        free(copy);
        return fd;
}

/* Open the directory rel below the base. "" is the base itself. */
static int open_dir_rel(struct receiver *r, const char *rel)
{
        char *final = NULL;
        int dirfd;
        int fd;

        if (r->base_fd < 0) {
                errno = ENOENT;
                return -1;
        }
        dirfd = walk_parent(r->base_fd, rel, &final);
        if (dirfd < 0) {
                return -1;
        }
        if (!final[0]) {
                free(final);
                return dirfd;
        }
        fd = openat(dirfd, final,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dirfd);
        free(final);
        return fd;
}

static int root_error(struct receiver *r, const char *msg)
{
        report(r, A_ERROR, r->root, msg);
        return -1;
}

static int open_root_dir(struct receiver *r)
{
        int fd;

        fd = open(r->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
                return -1;
        }
        r->base_fd = fd;
        r->base = xstrdup(r->root);
        return 0;
}

/* NULL when the directory fd is on dm-era, or why it is not. */
static const char *dir_device_problem(int fd)
{
        struct stat st;

        if (fstat(fd, &st) < 0) {
                return strerror(errno);
        }
        return era_device_problem(st.st_dev);
}

/* The destination of a directory does not exist yet. */
static int create_root(struct receiver *r)
{
        struct stat st;
        char *parent;
        char *leaf;
        const char *why = NULL;

        split_path(r->root, &parent, &leaf);
        if (stat(parent, &st) < 0) {
                why = strerror(errno);
        } else {
                why = era_device_problem(st.st_dev);
        }
        free(parent);
        free(leaf);
        if (why) {
                return root_error(r, why);
        }

        if (opt_dry_run) {
                r->dry_missing = 1;
        } else {
                if (mkdir(r->root, 0700) < 0) {
                        return root_error(r, strerror(errno));
                }
                if (open_root_dir(r) < 0) {
                        return root_error(r, strerror(errno));
                }
        }
        report(r, A_CREATE, r->root, NULL);
        push_level(r)->created = 1;
        return 0;
}

int rcv_root(struct receiver *r, int src_is_dir, const char *src_name,
             char **name)
{
        struct stat st;
        const char *why;
        char *parent;
        char *leaf;
        int fd;

        *name = NULL;
        if (lstat(r->root, &st) == 0) {
                if (lstat_is_dir(r->root, &st)) {
                        if (open_root_dir(r) < 0) {
                                return root_error(r, strerror(errno));
                        }
                        why = dir_device_problem(r->base_fd);
                        if (why) {
                                return root_error(r, why);
                        }
                        if (src_is_dir) {
                                push_level(r);
                                *name = xstrdup("");
                        } else {
                                *name = xstrdup(src_name);
                        }
                        return 0;
                }
                if (src_is_dir) {
                        return root_error(r, "not a directory");
                }
        } else if (errno != ENOENT) {
                return root_error(r, strerror(errno));
        } else if (src_is_dir) {
                if (create_root(r) < 0) {
                        return -1;
                }
                *name = xstrdup("");
                return 0;
        } else if (ends_with_slash(r->root)) {
                return root_error(r, "not a directory");
        }

        /* The destination names the file itself. */
        split_path(r->root, &parent, &leaf);
        if (bad_name(leaf)) {
                free(parent);
                free(leaf);
                return root_error(r, "not a file");
        }
        fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
                int saved_errno = errno;

                free(parent);
                free(leaf);
                return root_error(r, strerror(saved_errno));
        }
        r->base_fd = fd;
        r->base = parent;
        why = dir_device_problem(fd);
        if (why) {
                free(leaf);
                return root_error(r, why);
        }
        *name = leaf;
        return 0;
}

/* Whether --exclude keeps the destination entry name, at path, in dfd. */
static int dest_excluded(int dfd, const char *name, const char *path)
{
        struct stat st;
        int is_dir = 0;

        if (!opt_excludes.n) {
                return 0;
        }
        if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                is_dir = S_ISDIR(st.st_mode);
        }
        return patterns_match(&opt_excludes, path, is_dir);
}

/*
 * Delete the entries of dfd that are not in keep: unfinished copies
 * always, everything else only with --delete and when not excluded.
 */
static void delete_extra(struct receiver *r, int dfd, const char *rel,
                         const struct names *keep)
{
        struct names names;
        size_t i;

        if (list_names(dfd, &names) < 0) {
                report_error(r, rel, "read destination: %s", strerror(errno));
                return;
        }
        for (i = 0; i < names.n; i++) {
                const char *name = names.v[i];
                int temp = is_temp_name(name);
                char *path;

                if (names_has(keep, name)) {
                        continue;
                }
                if (!temp && !opt_delete) {
                        continue;
                }
                path = path_join(rel, name);
                if (!temp && dest_excluded(dfd, name, path)) {
                        free(path);
                        continue;
                }
                if (!opt_dry_run && remove_tree(dfd, name) < 0) {
                        report_error(r, path, "delete: %s", strerror(errno));
                } else {
                        report(r, A_DELETE, path,
                               temp && !opt_delete ? "stale temp" : NULL);
                }
                free(path);
        }
        names_free(&names);
}

/* The destination state of the regular file name in dfd. */
static void fill_state(struct receiver *r, int dfd, const char *rel,
                       const char *name, struct dest_state *s)
{
        char xattr[ERA_STATE_MAX];
        char *child;
        char *full;
        int fd;

        s->type = T_MISSING;
        if (fstatat(dfd, name, &s->st, AT_SYMLINK_NOFOLLOW) < 0) {
                return;
        }
        s->exists = 1;
        s->type = file_type(&s->st);
        if (s->type != T_REG || opt_dry_run) {
                return;
        }
        fd = open_read(dfd, name);
        if (fd < 0) {
                return;
        }

        child = path_join(rel, name);
        full = full_path(r, child);
        era_set_context(child, (unsigned long long)s->st.st_size);
        era_dest_ranges(fd, &s->st, full, &s->dirty);
        era_set_context(NULL, 0);
        if (era_get_xattr(fd, xattr, sizeof xattr) == 0) {
                s->xattr = xstrdup(xattr);
        }
        close(fd);
        free(full);
        free(child);
}

int rcv_list(struct receiver *r, const char *rel, int sweep,
             const struct list_entry *entries, size_t n,
             struct dest_state *states)
{
        struct names keep = { 0 };
        size_t i;
        int dfd;

        memset(states, 0, n * sizeof *states);
        if (parent_failed(r)) {
                return 1;
        }
        if (r->dry_missing) {
                return 0;
        }
        dfd = open_dir_rel(r, rel);
        if (dfd < 0) {
                /* A dry run lists the directories it would create. */
                if (opt_dry_run && (errno == ENOENT || errno == ENOTDIR)) {
                        return 0;
                }
                report_error(r, rel, "open directory: %s", strerror(errno));
                if (r->n_levels) {
                        r->levels[r->n_levels - 1].failed = 1;
                }
                return 1;
        }

        if (sweep) {
                for (i = 0; i < n; i++) {
                        names_add(&keep, entries[i].name);
                }
                names_sort(&keep);
                delete_extra(r, dfd, rel, &keep);
                names_free(&keep);
        }
        for (i = 0; i < n; i++) {
                if (entries[i].type == T_REG) {
                        fill_state(r, dfd, rel, entries[i].name, &states[i]);
                }
        }
        close(dfd);
        return 0;
}

void rcv_dir(struct receiver *r, const char *rel, const struct stat *st)
{
        struct dir_level *level;
        struct stat cur;
        char *final = NULL;
        int failed = parent_failed(r);
        int dirfd;
        int exists;

        (void)st;
        level = push_level(r);
        if (failed || bad_rel(rel) || !rel[0]) {
                level->failed = 1;
                return;
        }
        if (r->dry_missing) {
                report(r, A_CREATE, rel, NULL);
                return;
        }
        dirfd = walk_parent(r->base_fd, rel, &final);
        if (dirfd < 0) {
                if (opt_dry_run) {
                        report(r, A_CREATE, rel, NULL);
                } else {
                        report_error(r, rel, "open: %s", strerror(errno));
                        level->failed = 1;
                }
                return;
        }

        exists = fstatat(dirfd, final, &cur, AT_SYMLINK_NOFOLLOW) == 0;
        if (opt_dry_run) {
                if (!exists) {
                        report(r, A_CREATE, rel, NULL);
                } else if (!S_ISDIR(cur.st_mode)) {
                        report(r, A_UPDATE, rel, "directory");
                }
                goto out;
        }
        if (exists && !S_ISDIR(cur.st_mode)) {
                if (remove_tree(dirfd, final) < 0) {
                        report_error(r, rel, "replace: %s", strerror(errno));
                        level->failed = 1;
                        goto out;
                }
                exists = 0;
        }
        if (!exists) {
                if (mkdirat(dirfd, final, 0700) < 0 && errno != EEXIST) {
                        report_error(r, rel, "mkdir: %s", strerror(errno));
                        level->failed = 1;
                        goto out;
                }
                report(r, A_CREATE, rel, NULL);
                level->created = 1;
        }

out:
        close(dirfd);
        free(final);
}

void rcv_dir_done(struct receiver *r, const char *rel, const struct stat *st)
{
        struct dir_level level;
        struct stat cur;
        int differ;
        int fd;

        if (r->n_levels == 0) {
                return;
        }
        level = r->levels[--r->n_levels];
        if (level.failed || !st || opt_dry_run || r->dry_missing) {
                return;
        }
        fd = open_dir_rel(r, rel);
        if (fd < 0) {
                report_error(r, rel, "open directory: %s", strerror(errno));
                return;
        }
        if (fstat(fd, &cur) < 0) {
                report_error(r, rel, "stat directory: %s", strerror(errno));
                goto out;
        }
        differ = !same_meta(&cur, st, 0);
        if (differ && apply_meta_fd(fd, st, 1) < 0) {
                report_error(r, rel, "metadata: %s", strerror(errno));
                goto out;
        }
        if (fsync(fd) < 0) {
                report_error(r, rel, "fsync: %s", strerror(errno));
                goto out;
        }
        if (differ && !level.created) {
                report(r, A_META, rel, NULL);
        }

out:
        close(fd);
}

/* The detail shown for a copy: "[full copy[: why], ]sent / need / size". */
static void copy_detail(char *buf, size_t size, int full, const char *why,
                        unsigned long long sent, unsigned long long need,
                        unsigned long long file_size)
{
        if (!full) {
                snprintf(buf, size, "%llu / %llu / %llu",
                         sent, need, file_size);
        } else if (why && why[0]) {
                snprintf(buf, size, "full copy: %.80s, %llu / %llu / %llu",
                         why, sent, need, file_size);
        } else {
                snprintf(buf, size, "full copy, %llu / %llu / %llu",
                         sent, need, file_size);
        }
}

/*
 * Report a failed put, unless the sender gave up and reported it already.
 * discard drops content that has not been written.
 */
static void put_error(struct receiver *r, const struct put_req *req,
                      struct put_data *data, int discard)
{
        int saved_errno = errno;

        if (discard) {
                data->discard(data);
        }
        progress_end();
        if (saved_errno != ECANCELED) {
                report_error(r, req->rel, "%s: %s",
                             req->full ? "copy" : "update",
                             strerror(saved_errno));
        }
}

/*
 * Store the source record on fd, keeping the destination record of
 * old_name, the file that fd replaces. old_name may be NULL.
 */
static void set_source_record(int fd, const char *path, const char *state,
                              int dirfd, const char *old_name)
{
        int old_fd = -1;

        if (old_name) {
                old_fd = openat(dirfd, old_name,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        }
        era_set_source(fd, path, state, old_fd);
        if (old_fd >= 0) {
                close(old_fd);
        }
}

/*
 * Write the whole file into a temporary file and rename it into place,
 * so the destination is never half written.
 */
static void put_full(struct receiver *r, const struct put_req *req,
                     struct put_data *data, int dirfd, const char *final,
                     int existed, const struct stat *old)
{
        unsigned long long nbytes = 0;
        struct stat st;
        char tmp[64];
        char detail[256];
        char *path = NULL;
        int fd;

        if (existed && !S_ISREG(old->st_mode) &&
            remove_tree(dirfd, final) < 0) {
                put_error(r, req, data, 1);
                return;
        }
        snprintf(tmp, sizeof tmp, TEMP_PREFIX "%d.%d", (int)getpid(),
                 r->temp_seq++);
        fd = openat(dirfd, tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
                put_error(r, req, data, 1);
                return;
        }

        if (data->write(data, fd, &nbytes) < 0) {
                goto fail;
        }
        if (ftruncate(fd, req->st.st_size) < 0) {
                goto fail;
        }
        if (apply_meta_fd(fd, &req->st, req->set_mtime) < 0) {
                goto fail;
        }
        path = full_path(r, req->rel);
        if (req->state[0]) {
                set_source_record(fd, path, req->state, dirfd,
                                  existed ? final : NULL);
        }
        if (fsync(fd) < 0 || fstat(fd, &st) < 0) {
                goto fail;
        }
        if (renameat(dirfd, tmp, dirfd, final) < 0) {
                goto fail;
        }
        if (req->state[0]) {
                era_note(path, &st, req->state);
        }

        progress_end();
        copy_detail(detail, sizeof detail, 1, req->why, nbytes, req->need,
                    (unsigned long long)req->st.st_size);
        report(r, existed ? A_UPDATE : A_CREATE, req->rel, detail);
        goto out;

fail:
        put_error(r, req, data, 0);
        unlinkat(dirfd, tmp, 0);
out:
        close(fd);
        free(path);
}

/*
 * Update the dirty ranges in place. The old era records stay until the
 * new bytes are durable, so a crash before the xattr update retries from
 * those records.
 */
static void put_ranges(struct receiver *r, const struct put_req *req,
                       struct put_data *data, int dirfd, const char *final)
{
        unsigned long long nbytes = 0;
        struct stat st;
        char detail[256];
        char *path = NULL;
        int fd;

        fd = openat(dirfd, final, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
                put_error(r, req, data, 1);
                return;
        }
        if (ftruncate(fd, req->st.st_size) < 0) {
                put_error(r, req, data, 1);
                goto out;
        }
        if (data->write(data, fd, &nbytes) < 0 ||
            ftruncate(fd, req->st.st_size) < 0 ||
            apply_meta_fd(fd, &req->st, req->set_mtime) < 0 ||
            fsync(fd) < 0) {
                put_error(r, req, data, 0);
                goto out;
        }
        path = full_path(r, req->rel);
        era_set_source(fd, path, req->state, fd);
        if (fsync(fd) < 0 || fstat(fd, &st) < 0) {
                put_error(r, req, data, 0);
                goto out;
        }
        if (req->state[0]) {
                era_note(path, &st, req->state);
        }

        progress_end();
        copy_detail(detail, sizeof detail, 0, NULL, nbytes, req->need,
                    (unsigned long long)req->st.st_size);
        report(r, A_UPDATE, req->rel, detail);

out:
        close(fd);
        free(path);
}

int rcv_put(struct receiver *r, const struct put_req *req,
            struct put_data *data)
{
        struct stat st;
        const char *why;
        char *final = NULL;
        int dirfd;
        int existed;

        if (bad_rel(req->rel) || !req->rel[0]) {
                data->discard(data);
                report_error(r, req->rel, "invalid path");
                return data->broken ? -1 : 0;
        }
        if (opt_dry_run || r->dry_missing) {
                data->discard(data);
                report(r, req->full ? A_CREATE : A_UPDATE, req->rel, req->why);
                return data->broken ? -1 : 0;
        }

        if (r->client) {
                progress_begin(req->rel, (unsigned long long)req->st.st_size,
                               req->need);
        }
        dirfd = walk_parent(r->base_fd, req->rel, &final);
        if (dirfd < 0) {
                put_error(r, req, data, 1);
                return data->broken ? -1 : 0;
        }
        why = dir_device_problem(dirfd);
        if (why) {
                data->discard(data);
                progress_end();
                report_error(r, req->rel, "%s", why);
                goto out;
        }
        existed = fstatat(dirfd, final, &st, AT_SYMLINK_NOFOLLOW) == 0;
        if (req->full || !existed || !S_ISREG(st.st_mode)) {
                put_full(r, req, data, dirfd, final, existed, &st);
        } else {
                put_ranges(r, req, data, dirfd, final);
        }

out:
        progress_end();
        close(dirfd);
        free(final);
        return data->broken ? -1 : 0;
}

/*
 * The directory that holds rel, for an entry other than a regular file.
 * Returns -1 when the entry is done: a dry run without a parent reports
 * kind as created, and a real failure is reported as an error.
 */
static int entry_parent(struct receiver *r, const char *rel,
                        const char *kind, char **final)
{
        int dirfd;

        if (bad_rel(rel) || !rel[0]) {
                report_error(r, rel, "invalid path");
                return -1;
        }
        if (r->dry_missing) {
                report(r, A_CREATE, rel, kind);
                return -1;
        }
        dirfd = walk_parent(r->base_fd, rel, final);
        if (dirfd < 0) {
                if (opt_dry_run) {
                        report(r, A_CREATE, rel, kind);
                } else {
                        report_error(r, rel, "open: %s", strerror(errno));
                }
                return -1;
        }
        return dirfd;
}

void rcv_meta(struct receiver *r, const char *rel, const struct stat *st,
              int set_mtime)
{
        struct stat cur;
        char *final = NULL;
        char *full;
        int dirfd;
        int fd;

        if (opt_dry_run || r->dry_missing) {
                report(r, A_META, rel, NULL);
                return;
        }
        dirfd = entry_parent(r, rel, NULL, &final);
        if (dirfd < 0) {
                return;
        }
        fd = openat(dirfd, final, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
                report_error(r, rel, "open: %s", strerror(errno));
                goto out;
        }
        if (apply_meta_fd(fd, st, set_mtime) < 0) {
                report_error(r, rel, "metadata: %s", strerror(errno));
                close(fd);
                goto out;
        }
        if (fstat(fd, &cur) == 0) {
                full = full_path(r, rel);
                era_arm(fd, full, &cur);
                free(full);
        }
        close(fd);
        report(r, A_META, rel, NULL);

out:
        close(dirfd);
        free(final);
}

void rcv_skip(struct receiver *r, const char *rel, const char *detail)
{
        struct stat cur;
        char *final = NULL;
        char *full;
        int dirfd;
        int fd;

        if (opt_dry_run || r->dry_missing) {
                report(r, A_SKIP, rel, detail);
                return;
        }
        dirfd = entry_parent(r, rel, NULL, &final);
        if (dirfd < 0) {
                return;
        }
        fd = openat(dirfd, final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
                if (fstat(fd, &cur) == 0) {
                        full = full_path(r, rel);
                        era_arm(fd, full, &cur);
                        free(full);
                }
                close(fd);
        }
        report(r, A_SKIP, rel, detail);
        close(dirfd);
        free(final);
}

void rcv_link(struct receiver *r, const char *rel, const char *target,
              const struct stat *st)
{
        struct stat cur;
        char *current = NULL;
        char *final = NULL;
        int dirfd;
        int exists;
        int same = 0;

        dirfd = entry_parent(r, rel, "symlink", &final);
        if (dirfd < 0) {
                return;
        }
        exists = fstatat(dirfd, final, &cur, AT_SYMLINK_NOFOLLOW) == 0;
        if (exists && S_ISLNK(cur.st_mode)) {
                current = read_link_at(dirfd, final);
                same = current && strcmp(current, target) == 0;
        }

        if (same) {
                if (opt_dry_run || same_meta(&cur, st, 1)) {
                        report(r, A_SKIP, rel, NULL);
                } else if (apply_owner_time_at(dirfd, final, st) < 0) {
                        report_error(r, rel, "metadata: %s", strerror(errno));
                } else {
                        report(r, A_META, rel, NULL);
                }
                goto out;
        }
        if (opt_dry_run) {
                report(r, exists ? A_UPDATE : A_CREATE, rel, "symlink");
                goto out;
        }

        if (exists && remove_tree(dirfd, final) < 0) {
                report_error(r, rel, "replace: %s", strerror(errno));
                goto out;
        }
        if (symlinkat(target, dirfd, final) < 0) {
                report_error(r, rel, "symlink: %s", strerror(errno));
                goto out;
        }
        if (apply_owner_time_at(dirfd, final, st) < 0) {
                report_error(r, rel, "metadata: %s", strerror(errno));
                goto out;
        }
        report(r, exists ? A_UPDATE : A_CREATE, rel, "symlink");

out:
        close(dirfd);
        free(final);
        free(current);
}

void rcv_node(struct receiver *r, const char *rel, const struct stat *st)
{
        struct stat cur;
        char *final = NULL;
        int dirfd;
        int exists;

        dirfd = entry_parent(r, rel, "special", &final);
        if (dirfd < 0) {
                return;
        }
        exists = fstatat(dirfd, final, &cur, AT_SYMLINK_NOFOLLOW) == 0;

        if (exists && same_special(&cur, st)) {
                if (opt_dry_run || same_meta(&cur, st, 0)) {
                        report(r, A_SKIP, rel, NULL);
                } else if (apply_meta_at(dirfd, final, st) < 0) {
                        report_error(r, rel, "metadata: %s", strerror(errno));
                } else {
                        report(r, A_META, rel, NULL);
                }
                goto out;
        }
        if (opt_dry_run) {
                report(r, exists ? A_UPDATE : A_CREATE, rel, "special");
                goto out;
        }

        if (exists && remove_tree(dirfd, final) < 0) {
                report_error(r, rel, "replace: %s", strerror(errno));
                goto out;
        }
        if (mknodat(dirfd, final, (st->st_mode & S_IFMT) | 0600,
                    st->st_rdev) < 0) {
                report_error(r, rel, "mknod: %s", strerror(errno));
                goto out;
        }
        if (apply_meta_at(dirfd, final, st) < 0) {
                report_error(r, rel, "metadata: %s", strerror(errno));
                goto out;
        }
        report(r, exists ? A_UPDATE : A_CREATE, rel, "special");

out:
        close(dirfd);
        free(final);
}

void rcv_done(struct receiver *r)
{
        if (opt_dry_run || r->base_fd < 0) {
                return;
        }
        if (fsync(r->base_fd) < 0) {
                report_error(r, "", "fsync: %s", strerror(errno));
        }
}
