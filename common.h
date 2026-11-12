/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_COMMON_H
#define ERASYNC_COMMON_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

extern int opt_verbose;
extern int opt_quiet;
extern int opt_dry_run;
extern int opt_delete;
extern volatile sig_atomic_t erasync_stop;

struct counters {
        unsigned long created;
        unsigned long updated;
        unsigned long meta;
        unsigned long unchanged;
        unsigned long deleted;
        unsigned long errors;
        unsigned long long bytes;
};
extern struct counters stats;

/* What was done to one entry. The values are part of the protocol. */
enum action {
        A_NONE = 0,
        A_CREATE,
        A_UPDATE,
        A_META,
        A_SKIP,
        A_DELETE,
        A_ERROR,
};

/*
 * --exclude and --force patterns, matched with fnmatch(3). A pattern
 * without a '/' matches an entry's name at any depth. One with a '/'
 * matches the end of the path below SRC, unless a leading '/' anchors it
 * at the top of SRC. A trailing '/' matches directories only.
 */
struct pattern {
        char *text;
        const char *glob;
        /* Match the path, not just the name. */
        int path;
        /* Match at the top of SRC only. */
        int anchored;
        int dir_only;
};

struct patterns {
        struct pattern *v;
        size_t n;
};

extern struct patterns opt_excludes;
extern struct patterns opt_forces;
/* --force without a pattern: copy every file whole. */
extern int opt_force_all;

void patterns_add(struct patterns *list, const char *text);

/* Whether rel, a path below SRC, matches one of the patterns. */
int patterns_match(const struct patterns *list, const char *rel, int is_dir);

/* Count and print one action. An A_ERROR prints detail as the error. */
void report_action(int action, const char *rel, const char *detail);

/* Count and print an error for rel. */
void err_rel(const char *rel, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Short size, such as "40G" or "1.2M". */
void format_bytes(char *buf, size_t n, unsigned long long bytes);

/*
 * One live line for the file being copied: bytes sent, bytes this copy
 * will read, and the whole file. expect is the second of those. Quiet
 * mode prints nothing.
 */
void progress_begin(const char *rel, unsigned long long file_size,
                    unsigned long long expect);
void progress_add(unsigned long long n);
void progress_end(void);

/*
 * Next piece of [off, end). Unwritten extents are holes: they read as
 * zeros, so the caller punches that range instead of reading it.
 * *n is the piece length, always > 0 when off < end. Returns 0, or -1.
 */
int span_next(int fd, off_t off, off_t end, int *is_hole, off_t *n);

#endif
