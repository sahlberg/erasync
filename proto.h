/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_PROTO_H
#define ERASYNC_PROTO_H

/*
 * Frames between the two sides of a remote copy.
 *
 * A frame is a 4-byte payload length, a 4-byte op and the payload.
 * Numbers are big-endian. A string is a 4-byte length and its bytes.
 * The ops and their payloads are described in remote.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

enum {
        PROTO_VERSION = 4,
        MAX_FRAME = 32 * 1024 * 1024,
        /* The size of the data in one SPAN. */
        CHUNK = 8 * 1024 * 1024,
};

enum {
        OP_HELLO = 1,
        OP_ROOT,
        OP_ROOT_REPLY,
        OP_LIST,
        OP_LIST_NAMES,
        OP_LIST_END,
        OP_STATE,
        OP_LIST_REPLY,
        OP_DIR,
        OP_DIR_DONE,
        OP_PUT,
        OP_SPAN,
        OP_PUT_END,
        OP_META,
        OP_SKIP,
        OP_LINK,
        OP_NODE,
        OP_REPORT,
        OP_RESULT,
        OP_DONE,
};

/* A frame payload being built or received. */
struct buf {
        unsigned char *p;
        size_t n;
        size_t cap;
};

/* A reader over a received payload. */
struct rd {
        const unsigned char *p;
        const unsigned char *end;
};

struct peer {
        int in;
        int out;
        /* This side is the client, which prints the actions. */
        int client;
        /*
         * This side is the sender. Its writes also read the RESULT frames
         * that come back, so that neither side blocks on a full pipe.
         */
        int sender;
        /* A read or write failed: the session is over. */
        int broken;
        unsigned char *chunk;
};

void peer_init(struct peer *p, int in, int out, int client);
void peer_free(struct peer *p);

/* The sender's writes do not block, so that results can be read. */
int peer_set_sender(struct peer *p);

void buf_free(struct buf *b);
void buf_add(struct buf *b, const void *p, size_t n);
void buf_u8(struct buf *b, unsigned int v);
void buf_u32(struct buf *b, uint32_t v);
void buf_u64(struct buf *b, uint64_t v);
void buf_str(struct buf *b, const char *s);
/* uid, gid and mtime. */
void buf_owner_time(struct buf *b, const struct stat *st);
/* Mode, uid, gid and mtime. */
void buf_meta(struct buf *b, const struct stat *st);

void rd_init(struct rd *r, const struct buf *b);
size_t rd_left(const struct rd *r);
int rd_u8(struct rd *r, unsigned int *v);
int rd_u32(struct rd *r, uint32_t *v);
int rd_u64(struct rd *r, uint64_t *v);
/* A string of at most max bytes. */
int rd_str(struct rd *r, char **out, size_t max);
int rd_owner_time(struct rd *r, struct stat *st);
int rd_meta(struct rd *r, struct stat *st);

/* Send a frame and free its payload. */
int send_buf(struct peer *p, unsigned int op, struct buf *b);
int send_op(struct peer *p, unsigned int op);

/*
 * Receive a frame. On the sender, RESULT frames are reported and skipped,
 * so this returns the next frame of any other kind.
 */
int recv_frame(struct peer *p, unsigned int *op, struct buf *b);

/* Send an action or error to be printed by the client. */
int send_result(struct peer *p, unsigned int op, int action,
                const char *rel, const char *detail);

#endif
