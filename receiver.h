/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_RECEIVER_H
#define ERASYNC_RECEIVER_H

/*
 * The receiving side of a copy: everything that writes to the destination.
 *
 * The sender walks the source tree and drives the receiver. For a local
 * copy it calls these functions directly. For a remote copy they are
 * called by the protocol loop on the destination host.
 *
 * The calls for one tree arrive in walk order:
 *
 *   rcv_root                 resolve the destination
 *   rcv_list                 the entries of a source directory
 *     rcv_put, rcv_meta, rcv_skip, rcv_link, rcv_node
 *     rcv_dir                a subdirectory, then its own rcv_list and
 *       ...                  entries
 *     rcv_dir_done           the subdirectory is done
 *   rcv_dir_done             the top directory is done
 *   rcv_done
 *
 * Every action and error is passed to the report callback.
 */

#include "era.h"

#include <stddef.h>
#include <sys/stat.h>

typedef void report_fn(void *ctx, int action, const char *rel,
                       const char *detail);

/* A directory between rcv_dir and rcv_dir_done. */
struct dir_level {
        int created;
        int failed;
};

struct receiver {
        /* The destination path given by the user. */
        char *root;
        /* The directory that relative paths start from, and its path. */
        int base_fd;
        char *base;
        /* A dry run into a destination root that does not exist. */
        int dry_missing;
        /* This side shows the progress of each file. */
        int client;
        int temp_seq;
        report_fn *report;
        void *report_ctx;
        struct dir_level *levels;
        size_t n_levels;
        size_t cap_levels;
};

/* An entry of a source directory. type is a T_* value. */
struct list_entry {
        const char *name;
        int type;
};

/* The destination state of a regular file, used to plan its update. */
struct dest_state {
        int exists;
        int type;
        /* Too many dirty ranges to send: copy the whole file. */
        int force_full;
        struct stat st;
        char *xattr;
        struct era_ranges dirty;
};

/* Write a regular file. */
struct put_req {
        const char *rel;
        /* Replace the whole file, rather than update the dirty ranges. */
        int full;
        int set_mtime;
        /* Mode, owner, mtime and size of the source file. */
        struct stat st;
        /* Bytes the sender will read. */
        unsigned long long need;
        /* The source record to store. */
        const char *state;
        /* Why the whole file is copied, or "". */
        const char *why;
};

/* Where the bytes of a put come from. */
struct put_data {
        /*
         * Write the content to fd and add the bytes written to *nbytes.
         * Returns 0, or -1 with errno set. ECANCELED means the sender
         * gave up and has reported why.
         */
        int (*write)(struct put_data *data, int fd,
                     unsigned long long *nbytes);
        /* Drop the content, when the put fails before writing it. */
        void (*discard)(struct put_data *data);
        /* Set when the content cannot be read: the session is over. */
        int broken;
};

void receiver_init(struct receiver *r, const char *root, int client,
                   report_fn *report, void *report_ctx);
void receiver_free(struct receiver *r);

/*
 * Resolve the destination for a source directory, or for the single file
 * src_name. A directory destination is created when missing. *name gets
 * the path of the single file below the base, or "" for a directory.
 * Returns 0, or -1 after reporting why the destination cannot be used.
 */
int rcv_root(struct receiver *r, int src_is_dir, const char *src_name,
             char **name);

/*
 * The entries of the source directory rel. With sweep, destination
 * entries that are not listed are deleted: unfinished copies always, the
 * rest only with --delete. states[i] is filled in for each T_REG entry.
 * Returns 0, or 1 when the directory cannot be used and its entries must
 * be skipped.
 */
int rcv_list(struct receiver *r, const char *rel, int sweep,
             const struct list_entry *entries, size_t n,
             struct dest_state *states);

void rcv_dir(struct receiver *r, const char *rel, const struct stat *st);

/*
 * The directory rel is done. Its metadata is set from st, unless st is
 * NULL because the source directory could not be read.
 */
void rcv_dir_done(struct receiver *r, const char *rel,
                  const struct stat *st);

/* Returns -1 only when data is broken. */
int rcv_put(struct receiver *r, const struct put_req *req,
            struct put_data *data);

void rcv_meta(struct receiver *r, const char *rel, const struct stat *st,
              int set_mtime);
void rcv_skip(struct receiver *r, const char *rel, const char *detail);
void rcv_link(struct receiver *r, const char *rel, const char *target,
              const struct stat *st);
void rcv_node(struct receiver *r, const char *rel, const struct stat *st);
void rcv_done(struct receiver *r);

void dest_state_free(struct dest_state *s);

#endif
