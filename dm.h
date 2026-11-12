/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_DM_H
#define ERASYNC_DM_H

/* Device-mapper tables and messages, and the block devices below them. */

#include <linux/dm-ioctl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct dm_tgt {
        uint64_t start;
        uint64_t len;
        char type[DM_MAX_TYPE_NAME + 1];
        char *params;
};

struct dm_table {
        dev_t dev;
        char id[DM_UUID_LEN + DM_NAME_LEN + 8];
        int n_targets;
        struct dm_tgt *targets;
};

/* Open /dev/mapper/control. Returns 0, or -1. */
int dm_open(void);
void dm_close(void);

/*
 * Read the targets of a device-mapper device. With DM_STATUS_TABLE_FLAG
 * the params of each target are its table line, without it they are its
 * status line.
 */
struct dm_table *dm_read_table(dev_t dev, uint32_t flags);
void dm_free_table(struct dm_table *tab);

/* The table of dev, read once and kept. */
struct dm_table *dm_get_table(dev_t dev);

/* The target that holds sector. */
struct dm_tgt *dm_find_target(struct dm_table *tab, uint64_t sector);

int dm_message(dev_t dev, const char *msg);

/* The device and start sector below a linear or crypt target. */
int dm_lower_device(const struct dm_tgt *t, dev_t *dev, uint64_t *sector);

/* Read the first line of /sys/dev/block/<dev>/<attr>. */
int sysfs_read(dev_t dev, const char *attr, char *buf, size_t len);

#endif
