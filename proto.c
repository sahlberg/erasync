/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "proto.h"

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void peer_init(struct peer *p, int in, int out, int client)
{
        memset(p, 0, sizeof *p);
        p->in = in;
        p->out = out;
        p->client = client;
        p->chunk = xmalloc(CHUNK);
}

void peer_free(struct peer *p)
{
        free(p->chunk);
        p->chunk = NULL;
}

int peer_set_sender(struct peer *p)
{
        int flags;

        p->sender = 1;
        flags = fcntl(p->out, F_GETFL);
        if (flags < 0) {
                return -1;
        }
        return fcntl(p->out, F_SETFL, flags | O_NONBLOCK);
}

static void put_u32(unsigned char *p, uint32_t v)
{
        p[0] = (unsigned char)(v >> 24);
        p[1] = (unsigned char)(v >> 16);
        p[2] = (unsigned char)(v >> 8);
        p[3] = (unsigned char)v;
}

static uint32_t get_u32(const unsigned char *p)
{
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                ((uint32_t)p[2] << 8) | p[3];
}

void buf_free(struct buf *b)
{
        free(b->p);
        memset(b, 0, sizeof *b);
}

static void buf_grow(struct buf *b, size_t n)
{
        if (n <= b->cap) {
                return;
        }
        while (b->cap < n) {
                b->cap = b->cap ? b->cap * 2 : 256;
        }
        b->p = xrealloc(b->p, b->cap);
}

void buf_add(struct buf *b, const void *p, size_t n)
{
        buf_grow(b, b->n + n);
        if (n) {
                memcpy(b->p + b->n, p, n);
        }
        b->n += n;
}

void buf_u8(struct buf *b, unsigned int v)
{
        unsigned char c = (unsigned char)v;

        buf_add(b, &c, 1);
}

void buf_u32(struct buf *b, uint32_t v)
{
        unsigned char c[4];

        put_u32(c, v);
        buf_add(b, c, 4);
}

void buf_u64(struct buf *b, uint64_t v)
{
        buf_u32(b, (uint32_t)(v >> 32));
        buf_u32(b, (uint32_t)v);
}

void buf_str(struct buf *b, const char *s)
{
        uint32_t n = (uint32_t)strlen(s);

        buf_u32(b, n);
        buf_add(b, s, n);
}

void buf_owner_time(struct buf *b, const struct stat *st)
{
        buf_u32(b, (uint32_t)st->st_uid);
        buf_u32(b, (uint32_t)st->st_gid);
        buf_u64(b, (uint64_t)st->st_mtim.tv_sec);
        buf_u64(b, (uint64_t)st->st_mtim.tv_nsec);
}

void buf_meta(struct buf *b, const struct stat *st)
{
        buf_u32(b, (uint32_t)st->st_mode);
        buf_owner_time(b, st);
}

void rd_init(struct rd *r, const struct buf *b)
{
        r->p = b->p;
        r->end = b->p + b->n;
}

size_t rd_left(const struct rd *r)
{
        return (size_t)(r->end - r->p);
}

int rd_u8(struct rd *r, unsigned int *v)
{
        if (rd_left(r) < 1) {
                return -1;
        }
        *v = *r->p++;
        return 0;
}

int rd_u32(struct rd *r, uint32_t *v)
{
        if (rd_left(r) < 4) {
                return -1;
        }
        *v = get_u32(r->p);
        r->p += 4;
        return 0;
}

int rd_u64(struct rd *r, uint64_t *v)
{
        uint32_t hi;
        uint32_t lo;

        if (rd_u32(r, &hi) < 0 || rd_u32(r, &lo) < 0) {
                return -1;
        }
        *v = ((uint64_t)hi << 32) | lo;
        return 0;
}

int rd_str(struct rd *r, char **out, size_t max)
{
        uint32_t n;

        *out = NULL;
        if (rd_u32(r, &n) < 0) {
                return -1;
        }
        if (n > max || rd_left(r) < n) {
                return -1;
        }
        *out = xmalloc((size_t)n + 1);
        memcpy(*out, r->p, n);
        (*out)[n] = '\0';
        r->p += n;
        return 0;
}

int rd_owner_time(struct rd *r, struct stat *st)
{
        uint32_t uid;
        uint32_t gid;
        uint64_t sec;
        uint64_t nsec;

        if (rd_u32(r, &uid) < 0 || rd_u32(r, &gid) < 0) {
                return -1;
        }
        if (rd_u64(r, &sec) < 0 || rd_u64(r, &nsec) < 0) {
                return -1;
        }
        st->st_uid = (uid_t)uid;
        st->st_gid = (gid_t)gid;
        st->st_mtim.tv_sec = (time_t)sec;
        st->st_mtim.tv_nsec = (long)nsec;
        return 0;
}

int rd_meta(struct rd *r, struct stat *st)
{
        uint32_t mode;

        if (rd_u32(r, &mode) < 0) {
                return -1;
        }
        st->st_mode = (mode_t)mode;
        return rd_owner_time(r, st);
}

static int read_full(struct peer *p, void *buf, size_t n)
{
        unsigned char *s = buf;
        ssize_t r;

        while (n) {
                if (erasync_stop) {
                        errno = EINTR;
                        return -1;
                }
                r = read(p->in, s, n);
                if (r < 0 && errno == EINTR) {
                        continue;
                }
                if (r <= 0) {
                        p->broken = 1;
                        return -1;
                }
                s += r;
                n -= (size_t)r;
        }
        return 0;
}

static int read_frame(struct peer *p, unsigned int *op, struct buf *b)
{
        unsigned char h[8];
        uint32_t n;

        buf_free(b);
        if (read_full(p, h, 8) < 0) {
                return -1;
        }
        n = get_u32(h);
        if (n > MAX_FRAME) {
                p->broken = 1;
                return -1;
        }
        buf_grow(b, n);
        if (n && read_full(p, b->p, n) < 0) {
                return -1;
        }
        b->n = n;
        *op = get_u32(h + 4);
        return 0;
}

/* Print the action in a RESULT frame. */
static int print_result(struct peer *p, struct buf *b)
{
        struct rd r;
        unsigned int action;
        char *rel = NULL;
        char *detail = NULL;
        int rc = -1;

        rd_init(&r, b);
        if (rd_u8(&r, &action) == 0 && rd_str(&r, &rel, PATH_MAX) == 0 &&
            rd_str(&r, &detail, PATH_MAX) == 0) {
                if (p->client) {
                        report_action((int)action, rel, detail);
                }
                rc = 0;
        }
        free(rel);
        free(detail);
        return rc;
}

/* Read and print one RESULT frame while the sender waits to write. */
static int drain_result(struct peer *p)
{
        struct buf b = { 0 };
        unsigned int op;
        int rc = -1;

        if (read_frame(p, &op, &b) == 0 && op == OP_RESULT) {
                rc = print_result(p, &b);
        }
        buf_free(&b);
        if (rc < 0) {
                p->broken = 1;
        }
        return rc;
}

/* Wait until out is writable, printing any results that arrive. */
static int wait_writable(struct peer *p)
{
        struct pollfd pfd[2];

        for (;;) {
                pfd[0].fd = p->out;
                pfd[0].events = POLLOUT;
                pfd[1].fd = p->in;
                pfd[1].events = POLLIN;
                if (poll(pfd, 2, -1) < 0) {
                        if (errno == EINTR && !erasync_stop) {
                                continue;
                        }
                        return -1;
                }
                if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                        if (drain_result(p) < 0) {
                                return -1;
                        }
                        continue;
                }
                if (pfd[0].revents) {
                        return 0;
                }
        }
}

static int write_full(struct peer *p, const void *buf, size_t n)
{
        const unsigned char *s = buf;
        ssize_t w;

        while (n) {
                if (erasync_stop) {
                        errno = EINTR;
                        return -1;
                }
                w = write(p->out, s, n);
                if (w < 0 && errno == EINTR) {
                        continue;
                }
                if (w < 0 && errno == EAGAIN && p->sender) {
                        if (wait_writable(p) < 0) {
                                p->broken = 1;
                                return -1;
                        }
                        continue;
                }
                if (w <= 0) {
                        p->broken = 1;
                        return -1;
                }
                s += w;
                n -= (size_t)w;
        }
        return 0;
}

int send_buf(struct peer *p, unsigned int op, struct buf *b)
{
        unsigned char h[8];
        int rc = -1;

        if (b->n > MAX_FRAME) {
                p->broken = 1;
                goto out;
        }
        put_u32(h, (uint32_t)b->n);
        put_u32(h + 4, (uint32_t)op);
        if (write_full(p, h, 8) < 0) {
                goto out;
        }
        if (b->n && write_full(p, b->p, b->n) < 0) {
                goto out;
        }
        rc = 0;

out:
        buf_free(b);
        return rc;
}

int send_op(struct peer *p, unsigned int op)
{
        struct buf b = { 0 };

        return send_buf(p, op, &b);
}

int recv_frame(struct peer *p, unsigned int *op, struct buf *b)
{
        for (;;) {
                if (read_frame(p, op, b) < 0) {
                        return -1;
                }
                if (!p->sender || *op != OP_RESULT) {
                        return 0;
                }
                if (print_result(p, b) < 0) {
                        p->broken = 1;
                        return -1;
                }
        }
}

int send_result(struct peer *p, unsigned int op, int action,
                const char *rel, const char *detail)
{
        struct buf b = { 0 };

        buf_u8(&b, (unsigned int)action);
        buf_str(&b, rel ? rel : "");
        buf_str(&b, detail ? detail : "");
        return send_buf(p, op, &b);
}
