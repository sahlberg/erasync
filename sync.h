/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_SYNC_H
#define ERASYNC_SYNC_H

/*
 * Make DEST a copy of the directory SRC, or copy one file, on this host.
 * A file is written to DEST, or into DEST when DEST is a directory.
 * Returns 0, or -1 if the copy could not be finished.
 */
int sync_tree(const char *src, const char *dst);

#endif
