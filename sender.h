/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_SENDER_H
#define ERASYNC_SENDER_H

/*
 * The sending side of a copy: walk the source tree, decide what each file
 * needs, and drive a destination. The destination is the receiver itself
 * for a local copy, or the remote protocol for a remote one.
 */

#include "era.h"
#include "receiver.h"

#include <sys/stat.h>

/* The bytes of a put: the whole file, or the listed ranges. */
struct put_source {
        int fd;
        off_t size;
        /* NULL for the whole file. */
        const struct era_ranges *ranges;
};

/*
 * The receiver's calls, as seen by the sender. Each returns -1 when the
 * session cannot go on. Problems with single entries are reported by the
 * receiver and do not stop the copy.
 */
struct dest_ops {
        /* Returns -1 also when the destination cannot be used. */
        int (*root)(void *ctx, int src_is_dir, const char *src_name,
                    char **name);
        /* Returns 0 or 1 as rcv_list does, or -1. */
        int (*list)(void *ctx, const char *rel, int sweep,
                    const struct list_entry *entries, size_t n,
                    struct dest_state *states);
        int (*dir)(void *ctx, const char *rel, const struct stat *st);
        int (*dir_done)(void *ctx, const char *rel, const struct stat *st);
        int (*put)(void *ctx, const struct put_req *req,
                   const struct put_source *src);
        int (*meta)(void *ctx, const char *rel, const struct stat *st,
                    int set_mtime);
        int (*skip)(void *ctx, const char *rel, const char *detail);
        int (*link)(void *ctx, const char *rel, const char *target,
                    const struct stat *st);
        int (*node)(void *ctx, const char *rel, const struct stat *st);
        /* An action or error found by the sender, such as in a dry run. */
        int (*report)(void *ctx, int action, const char *rel,
                      const char *detail);
        int (*done)(void *ctx);
};

struct dest {
        const struct dest_ops *ops;
        void *ctx;
};

/*
 * Copy the directory tree or single file src to the destination.
 * Returns 0, or -1 when the copy could not be finished.
 */
int send_all(struct dest *d, const char *src);

enum {
        PLAN_SKIP = 1,
        PLAN_META,
        PLAN_RANGES,
        PLAN_FULL
};

/* What to do with one regular file. dirty is owned by the caller. */
struct file_plan {
        int kind;
        int set_mtime;
        char state[ERA_STATE_MAX];
        char why[80];
        struct era_ranges dirty;
};

/*
 * Decide how to update one regular file. xattr is the destination
 * watermark, or "". dest_dirty was produced by era_dest_ranges. existed
 * is false when DEST is missing or not a regular file.
 */
void plan_file(int sfd, const struct stat *sst, int existed,
               const struct stat *dst, const char *xattr,
               const struct era_ranges *dest_dirty, struct file_plan *out);

#endif
