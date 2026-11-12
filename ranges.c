/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "ranges.h"

#include "common.h"

#include <stdlib.h>
#include <string.h>

/*
 * The ranges are kept sorted by offset. They never overlap and are never
 * adjacent: a range that touches another is merged into it.
 */

static off_t range_end(const struct era_ranges *d, int i)
{
        return d->off[i] + d->len[i];
}

static void grow_ranges(struct era_ranges *d)
{
        if (d->n < d->alloc) {
                return;
        }
        d->alloc = d->alloc ? d->alloc * 2 : 64;
        d->off = xrealloc(d->off, (size_t)d->alloc * sizeof (off_t));
        d->len = xrealloc(d->len, (size_t)d->alloc * sizeof (off_t));
}

/* Move ranges [from, n) to start at index to. */
static void move_ranges(struct era_ranges *d, int to, int from)
{
        size_t count = (size_t)(d->n - from);

        memmove(&d->off[to], &d->off[from], count * sizeof (off_t));
        memmove(&d->len[to], &d->len[from], count * sizeof (off_t));
        d->n += to - from;
}

void era_add_range(struct era_ranges *d, off_t off, off_t len)
{
        off_t end;
        int first;
        int last;

        if (len <= 0) {
                return;
        }
        if (off < 0) {
                if (len <= -off) {
                        return;
                }
                len += off;
                off = 0;
        }
        end = off + len;

        /* Ranges are usually added in order, so search from the end. */
        first = d->n;
        while (first > 0 && range_end(d, first - 1) >= off) {
                first--;
        }

        /* Merge with every range that touches [off, end). */
        last = first;
        while (last < d->n && d->off[last] <= end) {
                if (d->off[last] < off) {
                        off = d->off[last];
                }
                if (range_end(d, last) > end) {
                        end = range_end(d, last);
                }
                last++;
        }

        /* Replace ranges [first, last) with the merged range. */
        if (last == first) {
                grow_ranges(d);
                move_ranges(d, first + 1, first);
        } else {
                move_ranges(d, first + 1, last);
        }
        d->off[first] = off;
        d->len[first] = end - off;
}

void era_merge_ranges(struct era_ranges *d, const struct era_ranges *extra,
                      off_t limit)
{
        struct era_ranges out;
        int i = 0;
        int j = 0;

        if (!extra || !extra->n) {
                return;
        }
        memset(&out, 0, sizeof out);

        /* Take the ranges in offset order, so each is appended to out. */
        while (i < d->n || j < extra->n) {
                off_t off;
                off_t len;

                if (j >= extra->n ||
                    (i < d->n && d->off[i] <= extra->off[j])) {
                        off = d->off[i];
                        len = d->len[i];
                        i++;
                } else {
                        off = extra->off[j];
                        len = extra->len[j];
                        j++;
                }
                if (off < 0 || len <= 0 || off >= limit) {
                        continue;
                }
                if (len > limit - off) {
                        len = limit - off;
                }
                era_add_range(&out, off, len);
        }

        free(d->off);
        free(d->len);
        *d = out;
}

void era_free_ranges(struct era_ranges *d)
{
        free(d->off);
        free(d->len);
        memset(d, 0, sizeof *d);
}

unsigned long long era_span_bytes(const struct era_ranges *d)
{
        unsigned long long n = 0;
        int i;

        if (!d) {
                return 0;
        }
        for (i = 0; i < d->n; i++) {
                if (d->len[i] > 0) {
                        n += (unsigned long long)d->len[i];
                }
        }
        return n;
}
