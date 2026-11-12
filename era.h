/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_ERA_H
#define ERASYNC_ERA_H

/*
 * dm-era change tracking for one sync process.
 *
 * Each destination regular file may carry the xattr user.erasync.era:
 *   "<era> <time> <device-id> [<era> <time> <device-id>]"
 * The first record is the source era the bytes came from. The second, when
 * present, is the destination era after which a write on that device means
 * something other than erasync changed the copy. The comment at the top of
 * era.c describes when each record is written.
 */

#include "ranges.h"

#include <sys/stat.h>

#define ERA_XATTR "user.erasync.era"
#define ERA_STATE_MAX 1024

/*
 * Check that we run as root and that era_invalidate and device-mapper are
 * there. Returns 0, or -1 after printing what is missing, prefixed by
 * where, such as "remote: ".
 */
int era_init(const char *where);

/*
 * Drop a held metadata snapshot and kill era_invalidate. Safe to call
 * more than once, including before era_init.
 */
void era_shutdown(void);

/*
 * NULL when dev is a filesystem on a dm-era device, or why it is not.
 * Both the source and the destination of a copy must be on dm-era.
 */
const char *era_device_problem(dev_t dev);

/* Read the destination xattr. 0 when a value was stored in state. */
int era_get_xattr(int fd, char *state, size_t state_sz);

/*
 * Blocks of fd written on its destination era device after the destination
 * record. Leaves dirty empty when there is nothing to repair.
 */
void era_dest_ranges(int fd, const struct stat *st, const char *path,
                     struct era_ranges *dirty);

/*
 * Fill dirty with source byte ranges that may differ from a destination
 * whose xattr is state. On success, state_out receives the source record
 * to store and *mtime_ok is set. A false mtime_ok means a write may have
 * landed after the snapshot: the caller must not give the destination the
 * source mtime, or the next run's size+mtime check could skip the file.
 * Returns NULL, or the reason the whole file must be copied.
 */
const char *era_file_ranges(int fd, const struct stat *st, const char *state,
                            struct era_ranges *dirty, char *state_out,
                            size_t state_sz, int *mtime_ok);

/*
 * Source record to store after a whole-file copy, or "" when fd is not on
 * dm-era. The snapshot happens before the caller reads the file. *mtime_ok
 * follows the same rule as era_file_ranges.
 */
void era_state_for_copy(int fd, const struct stat *st,
                        char *state_out, size_t state_sz, int *mtime_ok);

/*
 * Store source as the source record. When old_fd, which may be fd itself
 * or -1, has a destination record, keep it after the new source record so
 * a crash before era_finish can still see writes since that era.
 */
void era_set_source(int fd, const char *path, const char *source,
                    int old_fd);

/*
 * Remember a file that needs a destination record from era_finish.
 * era_note is for a file whose content was written. state is its source
 * record only. era_arm is for a file whose content was not written. It
 * does nothing unless the destination is on dm-era and the xattr has no
 * destination record yet.
 */
void era_note(const char *path, const struct stat *st, const char *state);
void era_arm(int fd, const char *path, const struct stat *st);

/* Advance each destination era device once, and write the records. */
void era_finish(void);

/* File named in the next snapshot message. NULL clears it. */
void era_set_context(const char *rel, unsigned long long file_size);

#endif
