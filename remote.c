/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copy between a local tree and one remote tree.
 *
 * A remote path is reached by running "erasync --server" over ssh. The
 * side with the source files runs the sender, the other side runs the
 * receiver, and either the client or the server can be the sender. The
 * sender's calls into the receiver (see receiver.h) become frames:
 *
 *   HELLO       client to server: version, flags, the remote path, then
 *               the --exclude and the --force patterns, each list as a
 *               count and the patterns. The server answers with HELLO
 *               carrying its version.
 *   ROOT        src_is_dir, src_name. Answered by ROOT_REPLY: ok, name.
 *   LIST        rel, sweep, then LIST_NAMES frames of (name, type) and
 *               LIST_END. Answered by one STATE frame per regular file,
 *               in order, then LIST_REPLY: available.
 *   DIR         rel, meta
 *   DIR_DONE    rel, has_meta, meta
 *   PUT         rel, full, set_mtime, meta, size, need, state, why, then
 *               SPAN frames and PUT_END: status, 1 when the sender gave
 *               up. A SPAN is kind (0 hole, 1 data), offset, length and
 *               the data.
 *   META        rel, set_mtime, meta
 *   SKIP        rel, detail
 *   LINK        rel, target, uid, gid, mtime
 *   NODE        rel, mode, rdev, uid, gid, mtime
 *   REPORT      action, rel, detail: found by the sender, for the client
 *   DONE        answered by DONE
 *
 * Only ROOT, LIST and DONE wait for an answer. Everything else is sent
 * without waiting, so a directory costs one round trip however many
 * files it holds. The receiver handles the frames in order. When the
 * client is the sender, the receiver sends a RESULT frame (action, rel,
 * detail) for each action, and the sender prints them as they arrive.
 */
#include "remote.h"

#include "common.h"
#include "era.h"
#include "fsutil.h"
#include "proto.h"
#include "receiver.h"
#include "sender.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
        F_DELETE = 1,
        F_DRY = 2,
        F_SERVER_SENDS = 4,
        F_VERBOSE = 8,
        F_QUIET = 16,
        F_FORCE_ALL = 32,
        /* An older server ignores it, and checks every file anyway. */
        F_CHECK_DEST = 64,
        /* Ranges that fit in a STATE. More than this copies the file. */
        MAX_RANGES = (4 * 1024 * 1024) / 16,
        MAX_NAMES = 1000000,
        /* Payload of one LIST_NAMES frame. */
        NAMES_BATCH = 1024 * 1024,
        RSH_MAX_WORDS = 27,
        MAX_PATTERNS = 10000,
};

static char *rsh_cmd = "ssh";
static char *remote_program = "erasync";
static pid_t ssh_pid;

/* Count copied bytes on the client, which shows the progress. */
static void count_bytes(struct peer *p, unsigned long long n)
{
        if (!p->client) {
                return;
        }
        stats.bytes += n;
        progress_add(n);
}

static void buf_state(struct buf *b, const struct dest_state *s)
{
        uint32_t n = 0;
        uint32_t i;
        int force_full = s->force_full || s->dirty.n > MAX_RANGES;

        buf_u8(b, (unsigned int)s->exists);
        buf_u8(b, (unsigned int)s->type);
        buf_u8(b, (unsigned int)force_full);
        buf_meta(b, &s->st);
        buf_u64(b, (uint64_t)s->st.st_size);
        buf_str(b, s->xattr ? s->xattr : "");
        if (!force_full) {
                n = (uint32_t)s->dirty.n;
        }
        buf_u32(b, n);
        for (i = 0; i < n; i++) {
                buf_u64(b, (uint64_t)s->dirty.off[i]);
                buf_u64(b, (uint64_t)s->dirty.len[i]);
        }
}

static int rd_state(struct rd *r, struct dest_state *s)
{
        unsigned int exists;
        unsigned int type;
        unsigned int force;
        uint64_t size;
        uint32_t n;
        uint32_t i;

        if (rd_u8(r, &exists) < 0 || rd_u8(r, &type) < 0 ||
            rd_u8(r, &force) < 0) {
                return -1;
        }
        if (rd_meta(r, &s->st) < 0 || rd_u64(r, &size) < 0) {
                return -1;
        }
        if (rd_str(r, &s->xattr, ERA_STATE_MAX) < 0 || rd_u32(r, &n) < 0) {
                return -1;
        }
        if (n > MAX_RANGES) {
                return -1;
        }
        s->exists = (int)exists;
        s->type = (int)type;
        s->force_full = (int)force;
        s->st.st_size = (off_t)size;
        for (i = 0; i < n; i++) {
                uint64_t off;
                uint64_t len;

                if (rd_u64(r, &off) < 0 || rd_u64(r, &len) < 0) {
                        return -1;
                }
                era_add_range(&s->dirty, (off_t)off, (off_t)len);
        }
        return 0;
}

/*
 * The sending side: a destination reached through the peer.
 */

/* Wait for a frame of kind want. */
static int expect_frame(struct peer *p, unsigned int want, struct buf *b)
{
        unsigned int op;

        if (recv_frame(p, &op, b) < 0) {
                return -1;
        }
        if (op != want) {
                fprintf(stderr, "erasync: remote protocol error\n");
                p->broken = 1;
                return -1;
        }
        return 0;
}

static int remote_root(void *ctx, int src_is_dir, const char *src_name,
                       char **name)
{
        struct peer *p = ctx;
        struct buf b = { 0 };
        struct rd r;
        unsigned int ok;
        int rc = -1;

        buf_u8(&b, (unsigned int)src_is_dir);
        buf_str(&b, src_name);
        if (send_buf(p, OP_ROOT, &b) < 0) {
                return -1;
        }
        if (expect_frame(p, OP_ROOT_REPLY, &b) < 0) {
                goto out;
        }
        rd_init(&r, &b);
        if (rd_u8(&r, &ok) < 0 || rd_str(&r, name, PATH_MAX) < 0) {
                goto out;
        }
        rc = ok ? 0 : -1;

out:
        buf_free(&b);
        return rc;
}

static int remote_list(void *ctx, const char *rel, int sweep,
                       const struct list_entry *entries, size_t n,
                       struct dest_state *states)
{
        struct peer *p = ctx;
        struct buf b = { 0 };
        struct buf names = { 0 };
        uint32_t count = 0;
        unsigned int avail;
        struct rd r;
        size_t i;
        int rc = -1;

        memset(states, 0, n * sizeof *states);
        buf_str(&b, rel);
        buf_u8(&b, (unsigned int)sweep);
        if (send_buf(p, OP_LIST, &b) < 0) {
                return -1;
        }
        for (i = 0; i < n; i++) {
                buf_str(&names, entries[i].name);
                buf_u8(&names, (unsigned int)entries[i].type);
                count++;
                if (names.n < NAMES_BATCH && i + 1 < n) {
                        continue;
                }
                buf_u32(&b, count);
                buf_add(&b, names.p, names.n);
                buf_free(&names);
                count = 0;
                if (send_buf(p, OP_LIST_NAMES, &b) < 0) {
                        return -1;
                }
        }
        if (send_op(p, OP_LIST_END) < 0) {
                return -1;
        }

        for (i = 0; i < n; i++) {
                if (entries[i].type != T_REG) {
                        continue;
                }
                if (expect_frame(p, OP_STATE, &b) < 0) {
                        goto out;
                }
                rd_init(&r, &b);
                if (rd_state(&r, &states[i]) < 0) {
                        goto out;
                }
        }
        if (expect_frame(p, OP_LIST_REPLY, &b) < 0) {
                goto out;
        }
        rd_init(&r, &b);
        if (rd_u8(&r, &avail) < 0) {
                goto out;
        }
        rc = avail ? 0 : 1;

out:
        buf_free(&b);
        return rc;
}

static int remote_dir(void *ctx, const char *rel, const struct stat *st)
{
        struct buf b = { 0 };

        buf_str(&b, rel);
        buf_meta(&b, st);
        return send_buf(ctx, OP_DIR, &b);
}

static int remote_dir_done(void *ctx, const char *rel, const struct stat *st)
{
        struct buf b = { 0 };
        struct stat none;

        memset(&none, 0, sizeof none);
        buf_str(&b, rel);
        buf_u8(&b, st != NULL);
        buf_meta(&b, st ? st : &none);
        return send_buf(ctx, OP_DIR_DONE, &b);
}

static int send_hole(struct peer *p, off_t off, off_t len)
{
        struct buf b = { 0 };

        buf_u8(&b, 0);
        buf_u64(&b, (uint64_t)off);
        buf_u64(&b, (uint64_t)len);
        return send_buf(p, OP_SPAN, &b);
}

static int send_data(struct peer *p, int fd, off_t off, off_t len)
{
        struct buf b = { 0 };
        ssize_t n;

        while (len > 0) {
                size_t chunk = len > CHUNK ? CHUNK : (size_t)len;

                n = pread(fd, p->chunk, chunk, off);
                if (n < 0 && errno == EINTR) {
                        continue;
                }
                if (n <= 0) {
                        if (n == 0) {
                                errno = EIO;
                        }
                        return -1;
                }
                buf_u8(&b, 1);
                buf_u64(&b, (uint64_t)off);
                buf_u64(&b, (uint64_t)n);
                buf_add(&b, p->chunk, (size_t)n);
                if (send_buf(p, OP_SPAN, &b) < 0) {
                        return -1;
                }
                count_bytes(p, (unsigned long long)n);
                off += n;
                len -= n;
        }
        return 0;
}

/* Send [off, off + len) of fd. Holes and unwritten extents become holes. */
static int send_span(struct peer *p, int fd, off_t off, off_t len)
{
        off_t end = off + len;
        off_t n;
        int hole;

        while (off < end) {
                if (erasync_stop) {
                        errno = EINTR;
                        return -1;
                }
                if (span_next(fd, off, end, &hole, &n) < 0) {
                        return -1;
                }
                if (n <= 0) {
                        errno = EIO;
                        return -1;
                }
                if (hole) {
                        if (send_hole(p, off, n) < 0) {
                                return -1;
                        }
                } else {
                        if (send_data(p, fd, off, n) < 0) {
                                return -1;
                        }
                }
                off += n;
        }
        return 0;
}

static int send_content(struct peer *p, const struct put_source *src)
{
        int i;

        if (!src->ranges) {
                return send_span(p, src->fd, 0, src->size);
        }
        for (i = 0; i < src->ranges->n; i++) {
                if (send_span(p, src->fd, src->ranges->off[i],
                              src->ranges->len[i]) < 0) {
                        return -1;
                }
        }
        return 0;
}

static int remote_report(void *ctx, int action, const char *rel,
                         const char *detail);

static int remote_put(void *ctx, const struct put_req *req,
                      const struct put_source *src)
{
        struct peer *p = ctx;
        struct buf b = { 0 };
        unsigned int status = 0;

        buf_str(&b, req->rel);
        buf_u8(&b, (unsigned int)req->full);
        buf_u8(&b, (unsigned int)req->set_mtime);
        buf_meta(&b, &req->st);
        buf_u64(&b, (uint64_t)req->st.st_size);
        buf_u64(&b, (uint64_t)req->need);
        buf_str(&b, req->state);
        buf_str(&b, req->why);
        if (send_buf(p, OP_PUT, &b) < 0) {
                return -1;
        }
        if (p->client) {
                progress_begin(req->rel, (unsigned long long)req->st.st_size,
                               req->need);
        }
        if (send_content(p, src) < 0) {
                if (p->broken) {
                        progress_end();
                        return -1;
                }
                /* The source could not be read. The receiver drops it. */
                status = 1;
                progress_end();
                if (remote_report(p, A_ERROR, req->rel, strerror(errno)) < 0) {
                        return -1;
                }
        }
        progress_end();
        buf_u8(&b, status);
        return send_buf(p, OP_PUT_END, &b);
}

static int remote_meta(void *ctx, const char *rel, const struct stat *st,
                       int set_mtime)
{
        struct buf b = { 0 };

        buf_str(&b, rel);
        buf_u8(&b, (unsigned int)set_mtime);
        buf_meta(&b, st);
        return send_buf(ctx, OP_META, &b);
}

static int remote_skip(void *ctx, const char *rel, const char *detail)
{
        struct buf b = { 0 };

        buf_str(&b, rel);
        buf_str(&b, detail ? detail : "");
        return send_buf(ctx, OP_SKIP, &b);
}

static int remote_link(void *ctx, const char *rel, const char *target,
                       const struct stat *st)
{
        struct buf b = { 0 };

        buf_str(&b, rel);
        buf_str(&b, target);
        buf_owner_time(&b, st);
        return send_buf(ctx, OP_LINK, &b);
}

static int remote_node(void *ctx, const char *rel, const struct stat *st)
{
        struct buf b = { 0 };

        buf_str(&b, rel);
        buf_u32(&b, (uint32_t)st->st_mode);
        buf_u64(&b, (uint64_t)st->st_rdev);
        buf_owner_time(&b, st);
        return send_buf(ctx, OP_NODE, &b);
}

/* The client prints it. When that is the other side, send it there. */
static int remote_report(void *ctx, int action, const char *rel,
                         const char *detail)
{
        struct peer *p = ctx;

        if (p->client) {
                report_action(action, rel, detail);
                return 0;
        }
        return send_result(p, OP_REPORT, action, rel, detail);
}

static int remote_done(void *ctx)
{
        struct peer *p = ctx;
        struct buf b = { 0 };
        int rc;

        if (send_op(p, OP_DONE) < 0) {
                return -1;
        }
        rc = expect_frame(p, OP_DONE, &b);
        buf_free(&b);
        return rc;
}

static const struct dest_ops remote_ops = {
        .root = remote_root,
        .list = remote_list,
        .dir = remote_dir,
        .dir_done = remote_dir_done,
        .put = remote_put,
        .meta = remote_meta,
        .skip = remote_skip,
        .link = remote_link,
        .node = remote_node,
        .report = remote_report,
        .done = remote_done,
};

static int run_sender(struct peer *p, const char *src)
{
        struct dest d;

        if (peer_set_sender(p) < 0) {
                return -1;
        }
        d.ops = &remote_ops;
        d.ctx = p;
        return send_all(&d, src);
}

/*
 * The receiving side: frames from the peer become receiver calls.
 */

/* The receiver's actions are printed by the client. */
static void receiver_report(void *ctx, int action, const char *rel,
                            const char *detail)
{
        struct peer *p = ctx;

        if (p->client) {
                report_action(action, rel, detail);
        } else {
                send_result(p, OP_RESULT, action, rel, detail);
        }
}

/* The content of a put, read from the SPAN frames that follow it. */
struct span_data {
        struct put_data base;
        struct peer *p;
};

static int apply_span(struct peer *p, int fd, struct buf *b,
                      unsigned long long *nbytes)
{
        struct rd r;
        unsigned int kind;
        uint64_t off;
        uint64_t len;
        off_t zeroed = 0;

        rd_init(&r, b);
        if (rd_u8(&r, &kind) < 0 || rd_u64(&r, &off) < 0 ||
            rd_u64(&r, &len) < 0) {
                errno = EPROTO;
                return -1;
        }
        if (kind == 0) {
                if (punch_hole(fd, (off_t)off, (off_t)len, &zeroed) < 0) {
                        return -1;
                }
                len = (uint64_t)zeroed;
        } else {
                if (rd_left(&r) != len) {
                        errno = EPROTO;
                        return -1;
                }
                if (pwrite_full(fd, r.p, (size_t)len, (off_t)off) < 0) {
                        return -1;
                }
        }
        *nbytes += len;
        count_bytes(p, len);
        return 0;
}

/* Read up to PUT_END. Returns its status, or -1 when the peer is gone. */
static int skip_spans(struct peer *p)
{
        struct buf b = { 0 };
        unsigned int op;
        unsigned int status;
        struct rd r;

        for (;;) {
                if (recv_frame(p, &op, &b) < 0) {
                        break;
                }
                if (op == OP_SPAN) {
                        continue;
                }
                if (op != OP_PUT_END) {
                        p->broken = 1;
                        break;
                }
                rd_init(&r, &b);
                if (rd_u8(&r, &status) < 0) {
                        p->broken = 1;
                        break;
                }
                buf_free(&b);
                return (int)status;
        }
        buf_free(&b);
        return -1;
}

static int span_write(struct put_data *data, int fd,
                      unsigned long long *nbytes)
{
        struct span_data *sd = (struct span_data *)data;
        struct peer *p = sd->p;
        struct buf b = { 0 };
        unsigned int op;
        int saved_errno;
        int status;

        for (;;) {
                if (recv_frame(p, &op, &b) < 0) {
                        data->broken = 1;
                        return -1;
                }
                if (op == OP_PUT_END) {
                        status = b.n ? b.p[0] : 0;
                        buf_free(&b);
                        if (status) {
                                errno = ECANCELED;
                                return -1;
                        }
                        return 0;
                }
                if (op != OP_SPAN) {
                        buf_free(&b);
                        p->broken = 1;
                        data->broken = 1;
                        errno = EPROTO;
                        return -1;
                }
                if (apply_span(p, fd, &b, nbytes) < 0) {
                        saved_errno = errno;
                        buf_free(&b);
                        status = skip_spans(p);
                        if (status < 0) {
                                data->broken = 1;
                        }
                        /* The sender's own error is already reported. */
                        errno = status > 0 ? ECANCELED : saved_errno;
                        return -1;
                }
        }
}

static void span_discard(struct put_data *data)
{
        struct span_data *sd = (struct span_data *)data;

        if (skip_spans(sd->p) < 0) {
                data->broken = 1;
        }
}

static int handle_root(struct peer *p, struct receiver *r, struct buf *in)
{
        struct buf b = { 0 };
        struct rd rd;
        unsigned int src_is_dir;
        char *src_name = NULL;
        char *name = NULL;
        int ok;

        rd_init(&rd, in);
        if (rd_u8(&rd, &src_is_dir) < 0 ||
            rd_str(&rd, &src_name, NAME_MAX) < 0) {
                return -1;
        }
        ok = rcv_root(r, (int)src_is_dir, src_name, &name) == 0;
        buf_u8(&b, (unsigned int)ok);
        buf_str(&b, ok ? name : "");
        free(src_name);
        free(name);
        return send_buf(p, OP_ROOT_REPLY, &b);
}

/* Free the names collected for a LIST. */
static void free_entries(struct list_entry *entries, size_t n)
{
        size_t i;

        for (i = 0; i < n; i++) {
                free((char *)entries[i].name);
        }
        free(entries);
}

/* Collect the LIST_NAMES frames of a LIST, up to LIST_END. */
static int read_list_names(struct peer *p, struct list_entry **entries,
                           size_t *n)
{
        struct buf b = { 0 };
        size_t cap = 0;
        unsigned int op;
        unsigned int type;
        uint32_t count;
        uint32_t i;
        struct rd r;
        char *name;

        *entries = NULL;
        *n = 0;
        for (;;) {
                if (recv_frame(p, &op, &b) < 0) {
                        goto fail;
                }
                if (op == OP_LIST_END) {
                        buf_free(&b);
                        return 0;
                }
                if (op != OP_LIST_NAMES) {
                        goto fail;
                }
                rd_init(&r, &b);
                if (rd_u32(&r, &count) < 0 || *n + count > MAX_NAMES) {
                        goto fail;
                }
                for (i = 0; i < count; i++) {
                        if (rd_str(&r, &name, NAME_MAX) < 0) {
                                goto fail;
                        }
                        if (bad_name(name) || rd_u8(&r, &type) < 0) {
                                free(name);
                                goto fail;
                        }
                        if (*n == cap) {
                                cap = cap ? cap * 2 : 64;
                                *entries = xrealloc(*entries,
                                                    cap * sizeof **entries);
                        }
                        (*entries)[*n].name = name;
                        (*entries)[*n].type = (int)type;
                        (*n)++;
                }
        }

fail:
        buf_free(&b);
        free_entries(*entries, *n);
        *entries = NULL;
        return -1;
}

static int handle_list(struct peer *p, struct receiver *r, struct buf *in)
{
        struct list_entry *entries;
        struct dest_state *states;
        struct buf b = { 0 };
        unsigned int sweep;
        char *rel = NULL;
        struct rd rd;
        size_t n;
        size_t i;
        int avail;
        int rc = -1;

        rd_init(&rd, in);
        if (rd_str(&rd, &rel, PATH_MAX) < 0 || rd_u8(&rd, &sweep) < 0) {
                free(rel);
                return -1;
        }
        if (read_list_names(p, &entries, &n) < 0) {
                free(rel);
                return -1;
        }

        states = xmalloc((n + 1) * sizeof *states);
        avail = rcv_list(r, rel, (int)sweep, entries, n, states);
        for (i = 0; i < n; i++) {
                if (entries[i].type != T_REG) {
                        continue;
                }
                buf_state(&b, &states[i]);
                if (send_buf(p, OP_STATE, &b) < 0) {
                        goto out;
                }
        }
        buf_u8(&b, avail == 0);
        rc = send_buf(p, OP_LIST_REPLY, &b);

out:
        for (i = 0; i < n; i++) {
                dest_state_free(&states[i]);
        }
        free(states);
        free_entries(entries, n);
        free(rel);
        return rc;
}

static int handle_put(struct peer *p, struct receiver *r, struct buf *in)
{
        struct span_data data;
        struct put_req req;
        unsigned int full;
        unsigned int set_mtime;
        uint64_t size;
        uint64_t need;
        char *rel = NULL;
        char *state = NULL;
        char *why = NULL;
        struct rd rd;
        int rc = -1;

        memset(&req, 0, sizeof req);
        rd_init(&rd, in);
        if (rd_str(&rd, &rel, PATH_MAX) < 0 || rd_u8(&rd, &full) < 0 ||
            rd_u8(&rd, &set_mtime) < 0 || rd_meta(&rd, &req.st) < 0 ||
            rd_u64(&rd, &size) < 0 || rd_u64(&rd, &need) < 0 ||
            rd_str(&rd, &state, ERA_STATE_MAX) < 0 ||
            rd_str(&rd, &why, PATH_MAX) < 0) {
                goto out;
        }
        req.rel = rel;
        req.full = (int)full;
        req.set_mtime = (int)set_mtime;
        req.st.st_size = (off_t)size;
        req.need = need;
        req.state = state;
        req.why = why;

        memset(&data, 0, sizeof data);
        data.base.write = span_write;
        data.base.discard = span_discard;
        data.p = p;
        rc = rcv_put(r, &req, &data.base);

out:
        free(rel);
        free(state);
        free(why);
        return rc;
}

/* DIR, DIR_DONE, META, SKIP, LINK, NODE and REPORT. */
static int handle_entry(struct receiver *r, unsigned int op,
                        struct buf *in)
{
        struct stat st;
        struct rd rd;
        unsigned int flag;
        uint32_t mode;
        uint64_t rdev;
        char *rel = NULL;
        char *text = NULL;
        int rc = -1;

        memset(&st, 0, sizeof st);
        rd_init(&rd, in);
        if (op == OP_REPORT) {
                if (rd_u8(&rd, &flag) < 0 ||
                    rd_str(&rd, &rel, PATH_MAX) < 0 ||
                    rd_str(&rd, &text, PATH_MAX) < 0) {
                        goto out;
                }
                r->report(r->report_ctx, (int)flag, rel, text);
                rc = 0;
                goto out;
        }

        if (rd_str(&rd, &rel, PATH_MAX) < 0) {
                goto out;
        }
        switch (op) {
        case OP_DIR:
                if (rd_meta(&rd, &st) < 0) {
                        goto out;
                }
                rcv_dir(r, rel, &st);
                break;
        case OP_DIR_DONE:
                if (rd_u8(&rd, &flag) < 0 || rd_meta(&rd, &st) < 0) {
                        goto out;
                }
                rcv_dir_done(r, rel, flag ? &st : NULL);
                break;
        case OP_META:
                if (rd_u8(&rd, &flag) < 0 || rd_meta(&rd, &st) < 0) {
                        goto out;
                }
                rcv_meta(r, rel, &st, (int)flag);
                break;
        case OP_SKIP:
                if (rd_str(&rd, &text, PATH_MAX) < 0) {
                        goto out;
                }
                rcv_skip(r, rel, text);
                break;
        case OP_LINK:
                if (rd_str(&rd, &text, PATH_MAX) < 0 ||
                    rd_owner_time(&rd, &st) < 0) {
                        goto out;
                }
                rcv_link(r, rel, text, &st);
                break;
        case OP_NODE:
                if (rd_u32(&rd, &mode) < 0 || rd_u64(&rd, &rdev) < 0 ||
                    rd_owner_time(&rd, &st) < 0) {
                        goto out;
                }
                st.st_mode = (mode_t)mode;
                st.st_rdev = (dev_t)rdev;
                rcv_node(r, rel, &st);
                break;
        default:
                goto out;
        }
        rc = 0;

out:
        free(rel);
        free(text);
        return rc;
}

/* Handle frames until DONE. Returns -1 when the session breaks. */
static int run_receiver(struct peer *p, struct receiver *r)
{
        struct buf b = { 0 };
        unsigned int op;
        int rc = 0;

        while (rc == 0) {
                if (erasync_stop || recv_frame(p, &op, &b) < 0) {
                        /* The sender went away, and said why if it could. */
                        buf_free(&b);
                        return -1;
                }
                switch (op) {
                case OP_ROOT:
                        rc = handle_root(p, r, &b);
                        break;
                case OP_LIST:
                        rc = handle_list(p, r, &b);
                        break;
                case OP_PUT:
                        rc = handle_put(p, r, &b);
                        break;
                case OP_DONE:
                        rcv_done(r);
                        buf_free(&b);
                        return send_op(p, OP_DONE);
                default:
                        rc = handle_entry(r, op, &b);
                        break;
                }
        }
        buf_free(&b);
        if (!erasync_stop) {
                fprintf(stderr, "erasync: %sremote protocol error\n",
                        p->client ? "" : "server: ");
        }
        return -1;
}

/*
 * Split cmd at spaces and tabs into av, which ends with a NULL. The words
 * point into *words, which the caller frees. Returns the number of words,
 * or -1 when there are none or more than max.
 */
static int split_words(const char *cmd, char **words, char **av, int max)
{
        char *s;
        int n = 0;

        *words = xstrdup(cmd);
        s = *words;
        for (;;) {
                while (*s == ' ' || *s == '\t') {
                        s++;
                }
                if (!*s) {
                        break;
                }
                if (n == max) {
                        return -1;
                }
                av[n++] = s;
                while (*s && *s != ' ' && *s != '\t') {
                        s++;
                }
                if (*s) {
                        *s++ = '\0';
                }
        }
        av[n] = NULL;
        return n ? n : -1;
}

/* Start "<rsh> host <remote program> --server" connected to p. */
static int spawn_ssh(struct peer *p, const char *host)
{
        char *av[RSH_MAX_WORDS + 4];
        char *words;
        int down[2] = { -1, -1 };
        int up[2] = { -1, -1 };
        pid_t pid;
        int n;

        n = split_words(rsh_cmd, &words, av, RSH_MAX_WORDS);
        if (n < 0) {
                free(words);
                return -1;
        }
        av[n++] = (char *)host;
        av[n++] = remote_program;
        av[n++] = "--server";
        av[n] = NULL;

        if (pipe(down) < 0 || pipe(up) < 0) {
                goto fail;
        }
        pid = fork();
        if (pid < 0) {
                goto fail;
        }
        if (pid == 0) {
                dup2(down[0], STDIN_FILENO);
                dup2(up[1], STDOUT_FILENO);
                close(down[0]);
                close(down[1]);
                close(up[0]);
                close(up[1]);
                execvp(av[0], av);
                fprintf(stderr, "erasync: cannot run %s: %s\n", av[0],
                        strerror(errno));
                _exit(127);
        }

        free(words);
        close(down[0]);
        close(up[1]);
        p->in = up[0];
        p->out = down[1];
        ssh_pid = pid;
        return 0;

fail:
        if (down[0] >= 0) {
                close(down[0]);
                close(down[1]);
        }
        if (up[0] >= 0) {
                close(up[0]);
                close(up[1]);
        }
        free(words);
        return -1;
}

static void buf_patterns(struct buf *b, const struct patterns *list)
{
        size_t i;

        buf_u32(b, (uint32_t)list->n);
        for (i = 0; i < list->n; i++) {
                buf_str(b, list->v[i].text);
        }
}

static int rd_patterns(struct rd *r, struct patterns *list)
{
        char *pattern;
        uint32_t n;
        uint32_t i;

        if (rd_u32(r, &n) < 0 || n > MAX_PATTERNS) {
                return -1;
        }
        for (i = 0; i < n; i++) {
                if (rd_str(r, &pattern, PATH_MAX) < 0) {
                        return -1;
                }
                patterns_add(list, pattern);
                free(pattern);
        }
        return 0;
}

/* Send HELLO and check the server's answer. */
static int client_hello(struct peer *p, int server_sends,
                        const char *remote_path)
{
        struct buf b = { 0 };
        uint32_t flags = 0;
        uint32_t version;
        unsigned int op;
        struct rd r;
        int rc = -1;

        if (opt_delete) {
                flags |= F_DELETE;
        }
        if (opt_dry_run) {
                flags |= F_DRY;
        }
        if (server_sends) {
                flags |= F_SERVER_SENDS;
        }
        if (opt_verbose) {
                flags |= F_VERBOSE;
        }
        if (opt_quiet) {
                flags |= F_QUIET;
        }
        if (opt_force_all) {
                flags |= F_FORCE_ALL;
        }
        if (opt_check_dest) {
                flags |= F_CHECK_DEST;
        }
        buf_u32(&b, PROTO_VERSION);
        buf_u32(&b, flags);
        buf_str(&b, remote_path);
        buf_patterns(&b, &opt_excludes);
        buf_patterns(&b, &opt_forces);
        if (send_buf(p, OP_HELLO, &b) < 0 ||
            recv_frame(p, &op, &b) < 0 || op != OP_HELLO) {
                fprintf(stderr, "erasync: cannot start remote erasync\n");
                goto out;
        }
        rd_init(&r, &b);
        if (rd_u32(&r, &version) < 0 || version != PROTO_VERSION) {
                fprintf(stderr, "erasync: remote erasync speaks protocol "
                        "%u, this one %u\n", version, PROTO_VERSION);
                goto out;
        }
        rc = 0;

out:
        buf_free(&b);
        return rc;
}

/* The remote path and the patterns of a HELLO. */
static int rd_hello_rest(struct rd *r, char **path)
{
        if (rd_str(r, path, PATH_MAX) < 0) {
                return -1;
        }
        if (rd_patterns(r, &opt_excludes) < 0) {
                return -1;
        }
        return rd_patterns(r, &opt_forces);
}

/* Receive HELLO on the server, and answer with our version. */
static int server_hello(struct peer *p, uint32_t *flags, char **path)
{
        struct buf b = { 0 };
        uint32_t version = 0;
        unsigned int op;
        struct rd r;
        int rc = -1;

        if (recv_frame(p, &op, &b) < 0 || op != OP_HELLO) {
                goto out;
        }
        rd_init(&r, &b);
        if (rd_u32(&r, &version) < 0 || rd_u32(&r, flags) < 0) {
                goto out;
        }
        if (version == PROTO_VERSION && rd_hello_rest(&r, path) < 0) {
                goto out;
        }
        buf_u32(&b, PROTO_VERSION);
        if (send_buf(p, OP_HELLO, &b) < 0) {
                goto out;
        }
        rc = version == PROTO_VERSION ? 0 : -1;

out:
        buf_free(&b);
        return rc;
}

void remote_set_rsh(const char *command)
{
        rsh_cmd = xstrdup(command);
}

void remote_set_program(const char *path)
{
        remote_program = xstrdup(path);
}

int remote_split(const char *spec, char **host, char **path)
{
        const char *colon;
        const char *slash;
        const char *bracket;

        if (!spec || !spec[0]) {
                return 0;
        }
        /* [host]:path, for an IPv6 address */
        if (spec[0] == '[') {
                bracket = strchr(spec, ']');
                if (!bracket || bracket[1] != ':') {
                        return 0;
                }
                *host = strndup(spec + 1, (size_t)(bracket - spec - 1));
                *path = xstrdup(bracket[2] ? bracket + 2 : ".");
                if (!*host) {
                        die("out of memory");
                }
                return 1;
        }

        slash = strchr(spec, '/');
        colon = strchr(spec, ':');
        if (!colon || colon == spec) {
                return 0;
        }
        if (slash && slash < colon) {
                return 0;
        }
        *host = strndup(spec, (size_t)(colon - spec));
        *path = xstrdup(colon[1] ? colon + 1 : ".");
        if (!*host) {
                die("out of memory");
        }
        return 1;
}

int remote_copy(const char *src, const char *dst, const char *src_host,
                const char *dst_host)
{
        struct receiver r;
        struct peer p;
        const char *host = src_host ? src_host : dst_host;
        const char *remote_path = src_host ? src : dst;
        int sending = src_host == NULL;
        int status = 0;
        int rc = -1;

        peer_init(&p, -1, -1, 1);
        signal(SIGPIPE, SIG_IGN);
        if (spawn_ssh(&p, host) < 0) {
                fprintf(stderr, "erasync: cannot start remote erasync\n");
                goto out;
        }
        if (client_hello(&p, !sending, remote_path) < 0) {
                goto out;
        }

        if (sending) {
                rc = run_sender(&p, src);
        } else {
                receiver_init(&r, dst, 1, receiver_report, &p);
                rc = run_receiver(&p, &r);
                receiver_free(&r);
        }

out:
        if (p.out >= 0) {
                close(p.out);
        }
        if (p.in >= 0) {
                close(p.in);
        }
        if (ssh_pid > 0) {
                waitpid(ssh_pid, &status, 0);
                if (rc == 0 &&
                    (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
                        fprintf(stderr, "erasync: remote erasync exited %d\n",
                                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                        rc = -1;
                }
                ssh_pid = 0;
        }
        peer_free(&p);
        return rc;
}

int remote_server(void)
{
        struct receiver r;
        struct peer p;
        uint32_t flags = 0;
        char *path = NULL;
        int rc;

        if (era_init("remote: ") < 0) {
                return -1;
        }
        peer_init(&p, STDIN_FILENO, STDOUT_FILENO, 0);
        signal(SIGPIPE, SIG_IGN);
        if (server_hello(&p, &flags, &path) < 0) {
                fprintf(stderr, "erasync: server: protocol version "
                        "mismatch\n");
                peer_free(&p);
                free(path);
                return -1;
        }
        opt_delete = (flags & F_DELETE) != 0;
        opt_dry_run = (flags & F_DRY) != 0;
        opt_verbose = (flags & F_VERBOSE) != 0;
        opt_quiet = (flags & F_QUIET) != 0;
        opt_force_all = (flags & F_FORCE_ALL) != 0;
        opt_check_dest = (flags & F_CHECK_DEST) != 0;

        if (flags & F_SERVER_SENDS) {
                rc = run_sender(&p, path);
        } else {
                receiver_init(&r, path, 0, receiver_report, &p);
                rc = run_receiver(&p, &r);
                receiver_free(&r);
        }
        if (rc == 0 && !erasync_stop) {
                era_finish();
        }
        free(path);
        peer_free(&p);
        return rc;
}
