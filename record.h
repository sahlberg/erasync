/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_RECORD_H
#define ERASYNC_RECORD_H

/*
 * The era records in the user.erasync.era xattr of a destination file:
 *
 *   v1 <era> <time> <device-id> [<era> <time> <device-id>]
 *
 * The version is only in the stored xattr. The state strings passed
 * around in the program start at the first record.
 */

#include <stddef.h>

#define RECORD_VERSION "v1 "

/*
 * Parse one "<era> <time> <id>" record and advance *pp past it.
 * stamp receives "<era> <time>" and id receives "<id>".
 * era, stamp and id may be NULL.
 * Returns 1 for a record, 0 at the end of the string, or -1 when the
 * record is malformed.
 */
int record_parse(const char **pp, unsigned long long *era,
                 char *stamp, size_t stamp_sz, char *id, size_t id_sz);

/* Whether state has a destination record, which is copied out. */
int record_has_dest(const char *state, char *stamp, size_t stamp_sz,
                    char *id, size_t id_sz);

/* Whether state is a source record and nothing else. */
int record_source_only(const char *state);

/* Format the record "<stamp> <id>" into out. */
int record_format(char *out, size_t out_sz, const char *stamp,
                  const char *id);

/* Store state in the xattr, or remove the xattr when state is empty. */
void record_store(int fd, const char *path, const char *state);

#endif
