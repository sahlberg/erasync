/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "../ranges.h"

#include <stdio.h>
#include <string.h>

static int fail;

static void expect(const char *what, int cond)
{
        if (!cond) {
                fprintf(stderr, "fail: %s\n", what);
                fail = 1;
        }
}

int main(void)
{
        struct era_ranges d, extra;

        memset(&d, 0, sizeof d);
        era_add_range(&d, 0, 10);
        era_add_range(&d, 20, 10);
        expect("two ranges", d.n == 2 && d.off[0] == 0 && d.len[0] == 10 &&
                d.off[1] == 20 && d.len[1] == 10);
        era_add_range(&d, 10, 10);
        expect("merge adjacent", d.n == 1 && d.off[0] == 0 && d.len[0] == 30);
        era_add_range(&d, 0, 0);
        era_add_range(&d, 5, -3);
        expect("ignore empty", d.n == 1 && d.len[0] == 30);
        era_free_ranges(&d);

        memset(&d, 0, sizeof d);
        era_add_range(&d, -5, 8);
        expect("clip negative", d.n == 1 && d.off[0] == 0 && d.len[0] == 3);
        era_free_ranges(&d);

        memset(&d, 0, sizeof d);
        memset(&extra, 0, sizeof extra);
        era_add_range(&d, 0, 10);
        era_add_range(&d, 50, 10);
        era_add_range(&extra, 5, 100);
        era_merge_ranges(&d, &extra, 40);
        /* [0,10) U [5,105) clipped to [0,40), and [50,60) dropped. */
        expect("merge and limit", d.n == 1 && d.off[0] == 0 && d.len[0] == 40);
        era_free_ranges(&d);
        era_free_ranges(&extra);

        memset(&d, 0, sizeof d);
        memset(&extra, 0, sizeof extra);
        era_merge_ranges(&d, &extra, 10);
        expect("merge nothing", d.n == 0);
        era_free_ranges(&d);

        return fail ? 1 : 0;
}
