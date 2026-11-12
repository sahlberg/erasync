/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_REMOTE_H
#define ERASYNC_REMOTE_H

/* 1 when spec is [user@]host:path (a colon before the first slash). */
int remote_split(const char *spec, char **host, char **path);

void remote_set_rsh(const char *command);
void remote_set_program(const char *path);

/* One of src_host and dst_host is set. Paths are local paths on that side. */
int remote_copy(const char *src, const char *dst, const char *src_host,
                const char *dst_host);

/* Speak the protocol on stdin/stdout. The peer sends the role and the path. */
int remote_server(void);

#endif
