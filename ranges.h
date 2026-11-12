/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_RANGES_H
#define ERASYNC_RANGES_H

#include <sys/types.h>

/* A sorted list of byte ranges. */
struct era_ranges {
        int n;
        int alloc;
        off_t *off;
        off_t *len;
};

/* Adjacent and overlapping ranges are merged. The list stays sorted. */
void era_add_range(struct era_ranges *d, off_t off, off_t len);

/* Merge a second sorted list into d, dropping anything outside [0, limit). */
void era_merge_ranges(struct era_ranges *d, const struct era_ranges *extra,
                      off_t limit);

void era_free_ranges(struct era_ranges *d);

/* Sum of the range lengths. */
unsigned long long era_span_bytes(const struct era_ranges *d);

#endif
