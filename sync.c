/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
/*
 * A local copy. The sender drives the receiver directly, and the bytes
 * are copied with copy_file_range where the filesystems allow it.
 */
#include "sync.h"

#include "common.h"
#include "fsutil.h"
#include "receiver.h"
#include "sender.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
        COPY_BUF_SIZE = 1024 * 1024,
        COPY_RANGE_CHUNK = 8 * 1024 * 1024,
};

static void count_copied(unsigned long long n)
{
        stats.bytes += n;
        progress_add(n);
}

static int copy_range_unsupported(int err)
{
        return err == EXDEV || err == EINVAL || err == ENOSYS ||
                err == EOPNOTSUPP || err == EBADF;
}

/* Copy one chunk with pread and pwrite. Returns the bytes copied. */
static ssize_t read_write_chunk(int sfd, int dfd, off_t off, size_t len)
{
        static char *buf;
        ssize_t n;

        if (!buf) {
                buf = xmalloc(COPY_BUF_SIZE);
        }
        if (len > COPY_BUF_SIZE) {
                len = COPY_BUF_SIZE;
        }
        do {
                n = pread(sfd, buf, len, off);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
                return -1;
        }
        if (n == 0) {
                errno = EIO;
                return -1;
        }
        if (pwrite_full(dfd, buf, (size_t)n, off) < 0) {
                return -1;
        }
        return n;
}

/* Copy [off, off + len) with copy_file_range, or pread and pwrite. */
static int copy_bytes(int sfd, int dfd, off_t off, off_t len)
{
        static int no_copy_range;

        while (len > 0) {
                size_t chunk = len > COPY_RANGE_CHUNK ? COPY_RANGE_CHUNK
                                                      : (size_t)len;
                ssize_t n;

                if (erasync_stop) {
                        errno = EINTR;
                        return -1;
                }
                if (no_copy_range) {
                        n = read_write_chunk(sfd, dfd, off, chunk);
                        if (n < 0) {
                                return -1;
                        }
                } else {
                        off_t in = off;
                        off_t out = off;

                        n = copy_file_range(sfd, &in, dfd, &out, chunk, 0);
                        if (n < 0 && errno == EINTR) {
                                continue;
                        }
                        if (n < 0 && copy_range_unsupported(errno)) {
                                no_copy_range = 1;
                                continue;
                        }
                        if (n <= 0) {
                                return -1;
                        }
                }
                off += n;
                len -= n;
                count_copied((unsigned long long)n);
        }
        return 0;
}

static int punch(int fd, off_t off, off_t len)
{
        off_t zeroed = 0;
        int rc;

        rc = punch_hole(fd, off, len, &zeroed);
        count_copied((unsigned long long)zeroed);
        return rc;
}

/* Copy one byte span. Holes and unwritten extents become holes. */
static int copy_span(int sfd, int dfd, off_t off, off_t len)
{
        off_t end = off + len;

        while (off < end) {
                int hole;
                off_t n;

                if (erasync_stop) {
                        errno = EINTR;
                        return -1;
                }
                if (span_next(sfd, off, end, &hole, &n) < 0) {
                        return -1;
                }
                if (n <= 0) {
                        errno = EIO;
                        return -1;
                }
                if (hole) {
                        if (punch(dfd, off, n) < 0) {
                                return -1;
                        }
                } else {
                        if (copy_bytes(sfd, dfd, off, n) < 0) {
                                return -1;
                        }
                }
                off += n;
        }
        return 0;
}

/* The content of a local put comes straight from the source file. */
struct local_data {
        struct put_data base;
        const struct put_source *src;
};

static int local_write(struct put_data *data, int fd,
                       unsigned long long *nbytes)
{
        const struct put_source *src = ((struct local_data *)data)->src;
        unsigned long long before = stats.bytes;
        int rc = 0;
        int i;

        if (!src->ranges) {
                rc = copy_span(src->fd, fd, 0, src->size);
        } else {
                for (i = 0; rc == 0 && i < src->ranges->n; i++) {
                        rc = copy_span(src->fd, fd, src->ranges->off[i],
                                       src->ranges->len[i]);
                }
        }
        *nbytes += stats.bytes - before;
        return rc;
}

static void local_discard(struct put_data *data)
{
        (void)data;
}

static int local_root(void *ctx, int src_is_dir, const char *src_name,
                      char **name)
{
        return rcv_root(ctx, src_is_dir, src_name, name);
}

static int local_list(void *ctx, const char *rel, int sweep,
                      const struct list_entry *entries, size_t n,
                      struct dest_state *states)
{
        return rcv_list(ctx, rel, sweep, entries, n, states);
}

static int local_dir(void *ctx, const char *rel, const struct stat *st)
{
        rcv_dir(ctx, rel, st);
        return 0;
}

static int local_dir_done(void *ctx, const char *rel, const struct stat *st)
{
        rcv_dir_done(ctx, rel, st);
        return 0;
}

static int local_put(void *ctx, const struct put_req *req,
                     const struct put_source *src)
{
        struct local_data data;

        memset(&data, 0, sizeof data);
        data.base.write = local_write;
        data.base.discard = local_discard;
        data.src = src;
        return rcv_put(ctx, req, &data.base);
}

static int local_meta(void *ctx, const char *rel, const struct stat *st,
                      int set_mtime)
{
        rcv_meta(ctx, rel, st, set_mtime);
        return 0;
}

static int local_skip(void *ctx, const char *rel, const char *detail)
{
        rcv_skip(ctx, rel, detail);
        return 0;
}

static int local_link(void *ctx, const char *rel, const char *target,
                      const struct stat *st)
{
        rcv_link(ctx, rel, target, st);
        return 0;
}

static int local_node(void *ctx, const char *rel, const struct stat *st)
{
        rcv_node(ctx, rel, st);
        return 0;
}

static int local_report(void *ctx, int action, const char *rel,
                        const char *detail)
{
        (void)ctx;
        report_action(action, rel, detail);
        return 0;
}

static int local_done(void *ctx)
{
        rcv_done(ctx);
        return 0;
}

static const struct dest_ops local_ops = {
        .root = local_root,
        .list = local_list,
        .dir = local_dir,
        .dir_done = local_dir_done,
        .put = local_put,
        .meta = local_meta,
        .skip = local_skip,
        .link = local_link,
        .node = local_node,
        .report = local_report,
        .done = local_done,
};

static void receiver_report(void *ctx, int action, const char *rel,
                            const char *detail)
{
        (void)ctx;
        report_action(action, rel, detail);
}

/* Whether one canonical path is the other, or below it. */
static int path_overlaps(const char *a, const char *b)
{
        size_t na = strlen(a);
        size_t nb = strlen(b);

        if (strcmp(a, b) == 0) {
                return 1;
        }
        if (na < nb) {
                return strncmp(b, a, na) == 0 &&
                        (a[na - 1] == '/' || b[na] == '/');
        }
        return strncmp(a, b, nb) == 0 && (b[nb - 1] == '/' || a[nb] == '/');
}

static int same_inode(const struct stat *a, const struct stat *b)
{
        return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/*
 * Whether the directory inner is outer, or below it, found by walking up
 * through "..". This also catches overlaps through bind mounts, which
 * path_overlaps cannot see. Returns 1, 0, or -1 when it cannot tell.
 */
static int dir_contains(int outer, int inner)
{
        struct stat outer_st;
        struct stat st;
        struct stat up_st;
        int rc = -1;
        int fd;
        int up;
        int i;

        if (fstat(outer, &outer_st) < 0) {
                return -1;
        }
        fd = dup(inner);
        if (fd < 0) {
                return -1;
        }
        for (i = 0; i < 256; i++) {
                if (fstat(fd, &st) < 0) {
                        break;
                }
                if (same_inode(&st, &outer_st)) {
                        rc = 1;
                        break;
                }
                up = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (up < 0) {
                        break;
                }
                if (fstat(up, &up_st) < 0) {
                        close(up);
                        break;
                }
                close(fd);
                fd = up;
                /* ".." of the root is the root itself. */
                if (same_inode(&up_st, &st)) {
                        rc = 0;
                        break;
                }
        }
        close(fd);
        return rc;
}

static char *canon(const char *path)
{
        char *r = realpath(path, NULL);

        if (!r) {
                die("%s: %s", path, strerror(errno));
        }
        return r;
}

/* Canonical path of a file or directory that may not exist yet. */
static char *canon_missing(const char *path)
{
        char *parent;
        char *name;
        char *real_parent;
        char *out;

        split_path(path, &parent, &name);
        if (!name[0]) {
                die("%s is not a directory", path);
        }
        real_parent = realpath(parent, NULL);
        if (!real_parent) {
                die("%s: %s", parent, strerror(errno));
        }
        out = path_join(real_parent, name);
        free(real_parent);
        free(parent);
        free(name);
        return out;
}

static int open_dir_or_die(const char *path)
{
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (fd < 0) {
                die("open %s: %s", path, strerror(errno));
        }
        return fd;
}

/* Refuse to copy a tree into itself, through paths or bind mounts. */
static void check_tree_overlap(const char *src, const char *dst)
{
        struct stat st;
        char *csrc = canon(src);
        char *cdst;
        char *parent;
        char *name;
        int sfd;
        int dfd;
        int contains;

        if (lstat(dst, &st) == 0) {
                cdst = canon(dst);
        } else if (errno == ENOENT) {
                cdst = canon_missing(dst);
        } else {
                die("%s: %s", dst, strerror(errno));
        }
        if (path_overlaps(csrc, cdst)) {
                die("source and destination overlap");
        }
        free(csrc);
        free(cdst);

        sfd = open_dir_or_die(src);
        if (lstat(dst, &st) == 0) {
                if (!lstat_is_dir(dst, &st)) {
                        close(sfd);
                        return;
                }
                dfd = open_dir_or_die(dst);
                contains = dir_contains(sfd, dfd);
                if (contains == 0) {
                        contains = dir_contains(dfd, sfd);
                }
        } else {
                /* A new directory overlaps when its parent is in src. */
                split_path(dst, &parent, &name);
                dfd = open_dir_or_die(parent);
                contains = dir_contains(sfd, dfd);
                free(parent);
                free(name);
        }
        close(dfd);
        close(sfd);
        if (contains < 0) {
                die("cannot tell whether source and destination overlap");
        }
        if (contains) {
                die("source and destination overlap");
        }
}

/* Refuse to copy a file onto itself. */
static void check_file_overlap(const char *src, const char *dst)
{
        struct stat sst;
        struct stat dst_st;
        char *target;
        char *csrc;
        char *cdst;
        char *parent;
        char *name;

        if (lstat(dst, &dst_st) == 0 && lstat_is_dir(dst, &dst_st)) {
                split_path(src, &parent, &name);
                target = path_join(dst, name);
                free(parent);
                free(name);
        } else if (ends_with_slash(dst)) {
                /* The receiver reports that this is not a directory. */
                return;
        } else {
                target = xstrdup(dst);
        }

        csrc = canon_missing(src);
        cdst = canon_missing(target);
        if (!strcmp(csrc, cdst)) {
                die("source and destination overlap");
        }
        if (lstat(src, &sst) == 0 && lstat(target, &dst_st) == 0 &&
            same_inode(&sst, &dst_st)) {
                die("source and destination overlap");
        }
        free(csrc);
        free(cdst);
        free(target);
}

int sync_tree(const char *src, const char *dst)
{
        struct receiver r;
        struct dest d;
        struct stat st;
        int rc;

        if (lstat(src, &st) < 0) {
                die("%s: %s", src, strerror(errno));
        }
        if (lstat_is_dir(src, &st)) {
                check_tree_overlap(src, dst);
        } else {
                check_file_overlap(src, dst);
        }

        receiver_init(&r, dst, 1, receiver_report, NULL);
        d.ops = &local_ops;
        d.ctx = &r;
        rc = send_all(&d, src);
        receiver_free(&r);
        if (rc < 0 || erasync_stop) {
                return -1;
        }
        return 0;
}
