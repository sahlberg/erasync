/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "common.h"

#include <errno.h>
#include <fnmatch.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int opt_verbose;
int opt_quiet;
int opt_dry_run;
int opt_delete;
volatile sig_atomic_t erasync_stop;
struct counters stats;

void die(const char *fmt, ...)
{
        va_list ap;

        fprintf(stderr, "erasync: ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        exit(2);
}

static void progress_hide(void);
static void progress_show(void);

struct patterns opt_excludes;
struct patterns opt_forces;
int opt_force_all;

void patterns_add(struct patterns *list, const char *text)
{
        struct pattern *pat;
        size_t len;
        char *glob;

        list->v = xrealloc(list->v, (list->n + 1) * sizeof *list->v);
        pat = &list->v[list->n];
        memset(pat, 0, sizeof *pat);
        pat->text = xstrdup(text);
        glob = xstrdup(text);
        len = strlen(glob);
        while (len > 1 && glob[len - 1] == '/') {
                glob[--len] = '\0';
                pat->dir_only = 1;
        }
        pat->path = strchr(glob, '/') != NULL;
        pat->anchored = glob[0] == '/';
        pat->glob = pat->anchored ? glob + 1 : glob;
        if (!pat->glob[0]) {
                die("empty pattern: '%s'", text);
        }
        list->n++;
}

/* Whether a pattern with a '/' matches rel, or the end of rel. */
static int path_matches(const struct pattern *pat, const char *rel)
{
        const char *s = rel;

        for (;;) {
                if (fnmatch(pat->glob, s, FNM_PATHNAME) == 0) {
                        return 1;
                }
                if (pat->anchored) {
                        return 0;
                }
                s = strchr(s, '/');
                if (!s) {
                        return 0;
                }
                s++;
        }
}

int patterns_match(const struct patterns *list, const char *rel, int is_dir)
{
        const char *slash = strrchr(rel, '/');
        const char *name = slash ? slash + 1 : rel;
        size_t i;

        for (i = 0; i < list->n; i++) {
                const struct pattern *pat = &list->v[i];

                if (pat->dir_only && !is_dir) {
                        continue;
                }
                if (pat->path) {
                        if (path_matches(pat, rel)) {
                                return 1;
                        }
                } else if (fnmatch(pat->glob, name, 0) == 0) {
                        return 1;
                }
        }
        return 0;
}

void err_rel(const char *rel, const char *fmt, ...)
{
        va_list ap;

        stats.errors++;
        progress_hide();
        fprintf(stderr, "erasync: %s: ", rel && rel[0] ? rel : ".");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        progress_show();
}

void report_action(int action, const char *rel, const char *detail)
{
        static const char *verbs[] = {
                [A_CREATE] = "create",
                [A_UPDATE] = "update",
                [A_META] = "meta",
                [A_SKIP] = "skip",
                [A_DELETE] = "delete",
        };
        const char *show = rel && rel[0] ? rel : ".";

        if (detail && !detail[0]) {
                detail = NULL;
        }
        switch (action) {
        case A_CREATE:
                stats.created++;
                break;
        case A_UPDATE:
                stats.updated++;
                break;
        case A_META:
                stats.meta++;
                break;
        case A_SKIP:
                stats.unchanged++;
                break;
        case A_DELETE:
                stats.deleted++;
                break;
        case A_ERROR:
                err_rel(rel, "%s", detail ? detail : "failed");
                return;
        default:
                return;
        }

        if (opt_quiet) {
                return;
        }
        if (action == A_SKIP && !opt_verbose) {
                return;
        }
        progress_hide();
        if (detail && opt_verbose) {
                printf("%s %s (%s)\n", verbs[action], show, detail);
        } else {
                printf("%s %s\n", verbs[action], show);
        }
        progress_show();
}

void *xmalloc(size_t n)
{
        void *p = calloc(1, n ? n : 1);

        if (!p) {
                die("out of memory");
        }
        return p;
}

void *xrealloc(void *p, size_t n)
{
        void *q = realloc(p, n ? n : 1);

        if (!q) {
                die("out of memory");
        }
        return q;
}

char *xstrdup(const char *s)
{
        char *p = strdup(s);

        if (!p) {
                die("out of memory");
        }
        return p;
}

void format_bytes(char *buf, size_t n, unsigned long long bytes)
{
        static const char *unit[] = { "B", "K", "M", "G", "T", "P" };
        double v = (double)bytes;
        int i = 0;

        while (v >= 1024.0 && i < 5) {
                v /= 1024.0;
                i++;
        }
        if (i == 0) {
                snprintf(buf, n, "%lluB", bytes);
        } else if (v >= 10.0) {
                snprintf(buf, n, "%.0f%s", v, unit[i]);
        } else {
                snprintf(buf, n, "%.1f%s", v, unit[i]);
        }
}

struct progress {
        int active;
        int tty;
        const char *rel;
        unsigned long long file_size;
        unsigned long long expect;
        unsigned long long sent;
        struct timespec start;
        struct timespec last;
};

static struct progress pg;

/* Redraw the progress line, at most every 0.2s unless force is set. */
static void progress_draw(int force)
{
        struct timespec now;
        double waited;

        if (!pg.active || opt_quiet) {
                return;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        waited = (double)(now.tv_sec - pg.last.tv_sec) +
                 (double)(now.tv_nsec - pg.last.tv_nsec) / 1e9;
        if (!force && waited < 0.2) {
                return;
        }
        pg.last = now;

        if (pg.tty) {
                fputc('\r', stderr);
        }
        /* bytes sent / bytes this copy will read / whole file */
        fprintf(stderr, "%s  %02d%% %llu / %llu / %llu",
                pg.rel, (int)(pg.sent * 100 / pg.expect),
                pg.sent, pg.expect, pg.file_size);
        if (pg.tty) {
                fputs("\033[K", stderr);
        } else {
                fputc('\n', stderr);
        }
        fflush(stderr);
}

void progress_begin(const char *rel, unsigned long long file_size,
                    unsigned long long expect)
{
        if (opt_quiet || !rel || !rel[0]) {
                return;
        }
        if (file_size == 0 && expect == 0) {
                return;
        }
        memset(&pg, 0, sizeof pg);
        pg.active = 1;
        pg.tty = isatty(STDERR_FILENO);
        pg.rel = rel;
        pg.file_size = file_size;
        pg.expect = expect;
        clock_gettime(CLOCK_MONOTONIC, &pg.start);
        pg.last = pg.start;
        progress_draw(1);
}

void progress_add(unsigned long long n)
{
        if (!pg.active || n == 0) {
                return;
        }
        pg.sent += n;
        progress_draw(0);
}

/*
 * Clear the progress line before printing another line, and draw it again
 * after. With pipelining, results for other files arrive while one file
 * is being copied.
 */
static void progress_hide(void)
{
        if (pg.active && pg.tty && !opt_quiet) {
                fputs("\r\033[K", stderr);
        }
}

static void progress_show(void)
{
        if (pg.active && pg.tty) {
                fflush(stdout);
                progress_draw(1);
        }
}

void progress_end(void)
{
        if (!pg.active) {
                return;
        }
        if (pg.tty) {
                fputs("\r\033[K", stderr);
        }
        fflush(stderr);
        pg.active = 0;
        pg.rel = NULL;
}

/* Set once FIEMAP turns out to be unsupported. */
static int span_use_seek;

static int span_set(int *is_hole, off_t *n, int hole, off_t len)
{
        *is_hole = hole;
        *n = len;
        return 0;
}

/* span_next() using SEEK_DATA and SEEK_HOLE. */
static int span_next_seek(int fd, off_t off, off_t end, int *is_hole,
                          off_t *n)
{
        off_t data;
        off_t hole;

        data = lseek(fd, off, SEEK_DATA);
        if (data < 0) {
                /* ENXIO: no data after off. EINVAL: no SEEK_DATA. */
                if (errno == ENXIO) {
                        return span_set(is_hole, n, 1, end - off);
                }
                if (errno == EINVAL) {
                        return span_set(is_hole, n, 0, end - off);
                }
                return -1;
        }
        if (data >= end) {
                return span_set(is_hole, n, 1, end - off);
        }
        if (data > off) {
                return span_set(is_hole, n, 1, data - off);
        }

        hole = lseek(fd, off, SEEK_HOLE);
        if (hole < 0) {
                if (errno == EINVAL) {
                        return span_set(is_hole, n, 0, end - off);
                }
                if (errno != ENXIO) {
                        return -1;
                }
                hole = end;
        }
        if (hole <= off || hole > end) {
                hole = end;
        }
        return span_set(is_hole, n, 0, hole - off);
}

int span_next(int fd, off_t off, off_t end, int *is_hole, off_t *n)
{
        struct {
                struct fiemap fm;
                struct fiemap_extent fe;
        } u;
        off_t ext_start;
        off_t ext_end;

        if (off >= end) {
                return span_set(is_hole, n, 1, 0);
        }
        if (span_use_seek) {
                return span_next_seek(fd, off, end, is_hole, n);
        }

        memset(&u, 0, sizeof u);
        u.fm.fm_start = (uint64_t)off;
        u.fm.fm_length = (uint64_t)(end - off);
        u.fm.fm_extent_count = 1;
        if (ioctl(fd, FS_IOC_FIEMAP, &u.fm) < 0) {
                if (errno != EOPNOTSUPP && errno != ENOTTY) {
                        return -1;
                }
                span_use_seek = 1;
                return span_next_seek(fd, off, end, is_hole, n);
        }
        if (u.fm.fm_mapped_extents == 0) {
                return span_set(is_hole, n, 1, end - off);
        }

        ext_start = (off_t)u.fe.fe_logical;
        ext_end = (off_t)(u.fe.fe_logical + u.fe.fe_length);
        if (ext_end <= ext_start) {
                errno = EIO;
                return -1;
        }
        if (ext_start > off) {
                if (ext_start > end) {
                        ext_start = end;
                }
                return span_set(is_hole, n, 1, ext_start - off);
        }
        if (ext_end > end) {
                ext_end = end;
        }
        if (ext_end <= off) {
                errno = EIO;
                return -1;
        }
        return span_set(is_hole, n,
                        (u.fe.fe_flags & FIEMAP_EXTENT_UNWRITTEN) != 0,
                        ext_end - off);
}
