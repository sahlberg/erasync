/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "dm.h"

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_TABLES 64

static int dm_fd = -1;
static struct dm_table *table_cache[MAX_TABLES];
static int n_tables;

int dm_open(void)
{
        if (dm_fd >= 0) {
                return 0;
        }
        dm_fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
        if (dm_fd < 0) {
                return -1;
        }
        return 0;
}

/* The kernel decodes dm_ioctl.dev with huge_decode_dev(). */
static uint64_t encode_dev(dev_t dev)
{
        return (minor(dev) & 0xff) | ((uint64_t)major(dev) << 8) |
                ((uint64_t)(minor(dev) & ~0xffU) << 12);
}

/*
 * Issue a device-mapper ioctl. The buffer is grown until the reply fits.
 * Returns the reply, which the caller frees, or NULL with errno set.
 */
static struct dm_ioctl *dm_call(unsigned long cmd, dev_t dev, uint32_t flags,
                                const void *payload, size_t payload_len)
{
        size_t size = 16384 + payload_len;
        struct dm_ioctl *io;

        if (dm_open() < 0) {
                return NULL;
        }
        for (;;) {
                io = xmalloc(size);
                io->version[0] = DM_VERSION_MAJOR;
                io->version[1] = DM_VERSION_MINOR;
                io->version[2] = DM_VERSION_PATCHLEVEL;
                io->data_size = size;
                io->data_start = sizeof *io;
                io->dev = encode_dev(dev);
                io->flags = flags;
                if (payload) {
                        memcpy((char *)io + io->data_start, payload,
                               payload_len);
                }

                if (ioctl(dm_fd, cmd, io) < 0) {
                        int saved_errno = errno;

                        free(io);
                        errno = saved_errno;
                        return NULL;
                }
                if (!(io->flags & DM_BUFFER_FULL_FLAG)) {
                        return io;
                }
                free(io);
                size *= 4;
        }
}

struct dm_table *dm_read_table(dev_t dev, uint32_t flags)
{
        struct dm_ioctl *io;
        struct dm_table *tab;
        char *base;
        uint32_t offset = 0;
        int i;

        io = dm_call(DM_TABLE_STATUS, dev, flags, NULL, 0);
        if (!io) {
                return NULL;
        }
        if (!(io->flags & DM_ACTIVE_PRESENT_FLAG)) {
                free(io);
                return NULL;
        }

        tab = xmalloc(sizeof *tab);
        tab->dev = dev;
        if (io->uuid[0]) {
                snprintf(tab->id, sizeof tab->id, "uuid-%s", io->uuid);
        } else {
                snprintf(tab->id, sizeof tab->id, "name-%s", io->name);
        }
        tab->n_targets = io->target_count;
        tab->targets = xmalloc((tab->n_targets + 1) * sizeof *tab->targets);

        base = (char *)io + io->data_start;
        for (i = 0; i < tab->n_targets; i++) {
                struct dm_target_spec *spec;
                struct dm_tgt *t = &tab->targets[i];

                spec = (struct dm_target_spec *)(base + offset);
                t->start = spec->sector_start;
                t->len = spec->length;
                memcpy(t->type, spec->target_type, DM_MAX_TYPE_NAME);
                t->type[DM_MAX_TYPE_NAME] = '\0';
                t->params = xstrdup((char *)(spec + 1));
                offset = spec->next;
        }
        free(io);
        return tab;
}

void dm_free_table(struct dm_table *tab)
{
        int i;

        if (!tab) {
                return;
        }
        for (i = 0; i < tab->n_targets; i++) {
                free(tab->targets[i].params);
        }
        free(tab->targets);
        free(tab);
}

int dm_message(dev_t dev, const char *msg)
{
        size_t len = sizeof (struct dm_target_msg) + strlen(msg) + 1;
        struct dm_target_msg *m = xmalloc(len);
        struct dm_ioctl *io;

        strcpy(m->message, msg);
        io = dm_call(DM_TARGET_MSG, dev, 0, m, len);
        free(m);
        if (!io) {
                return -1;
        }
        free(io);
        return 0;
}

struct dm_table *dm_get_table(dev_t dev)
{
        struct dm_table *tab;
        int i;

        for (i = 0; i < n_tables; i++) {
                if (table_cache[i]->dev == dev) {
                        return table_cache[i];
                }
        }
        if (n_tables == MAX_TABLES) {
                return NULL;
        }
        tab = dm_read_table(dev, DM_STATUS_TABLE_FLAG);
        if (!tab) {
                return NULL;
        }
        table_cache[n_tables++] = tab;
        return tab;
}

struct dm_tgt *dm_find_target(struct dm_table *tab, uint64_t sector)
{
        int i;

        for (i = 0; i < tab->n_targets; i++) {
                struct dm_tgt *t = &tab->targets[i];

                if (sector >= t->start && sector < t->start + t->len) {
                        return t;
                }
        }
        return NULL;
}

int sysfs_read(dev_t dev, const char *attr, char *buf, size_t len)
{
        char path[128];
        FILE *f;
        int ok;

        snprintf(path, sizeof path, "/sys/dev/block/%u:%u/%s",
                 major(dev), minor(dev), attr);
        f = fopen(path, "r");
        if (!f) {
                return 0;
        }
        ok = fgets(buf, len, f) != NULL;
        fclose(f);
        return ok;
}

int dm_lower_device(const struct dm_tgt *t, dev_t *dev, uint64_t *sector)
{
        unsigned int ma;
        unsigned int mi;
        unsigned long long start;
        int n;

        if (strcmp(t->type, "linear") == 0) {
                n = sscanf(t->params, "%u:%u %llu", &ma, &mi, &start);
        } else if (strcmp(t->type, "crypt") == 0) {
                n = sscanf(t->params, "%*s %*s %*s %u:%u %llu",
                           &ma, &mi, &start);
        } else {
                return 0;
        }
        if (n != 3) {
                return 0;
        }
        *dev = makedev(ma, mi);
        *sector = start;
        return 1;
}

void dm_close(void)
{
        int i;

        for (i = 0; i < n_tables; i++) {
                dm_free_table(table_cache[i]);
                table_cache[i] = NULL;
        }
        n_tables = 0;
        if (dm_fd >= 0) {
                close(dm_fd);
                dm_fd = -1;
        }
}
