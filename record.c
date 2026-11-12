/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "record.h"

#include "common.h"
#include "era.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>

int record_parse(const char **pp, unsigned long long *era,
                 char *stamp, size_t stamp_sz, char *id, size_t id_sz)
{
        const char *p = *pp;
        const char *id_start;
        const char *end;
        char *era_end;
        unsigned long long n;
        size_t stamp_len;
        size_t id_len;

        while (*p == ' ') {
                p++;
        }
        if (!*p) {
                return 0;
        }
        n = strtoull(p, &era_end, 10);
        if (era_end == p || *era_end != ' ' || n == 0) {
                return -1;
        }
        id_start = strchr(era_end + 1, ' ');
        if (!id_start) {
                return -1;
        }
        id_start++;
        end = strchrnul(id_start, ' ');
        stamp_len = id_start - 1 - p;
        id_len = end - id_start;
        if (id_len == 0) {
                return -1;
        }

        if (stamp) {
                if (stamp_len >= stamp_sz) {
                        return -1;
                }
                memcpy(stamp, p, stamp_len);
                stamp[stamp_len] = '\0';
        }

        if (id) {
                if (id_len >= id_sz) {
                        return -1;
                }
                memcpy(id, id_start, id_len);
                id[id_len] = '\0';
        }

        if (era) {
                *era = n;
        }
        *pp = end;
        return 1;
}

int record_has_dest(const char *state, char *stamp, size_t stamp_sz,
                    char *id, size_t id_sz)
{
        const char *p = state;

        /* Skip past the source record */
        if (record_parse(&p, NULL, NULL, 0, NULL, 0) != 1) {
                return 0;
        }

        return record_parse(&p, NULL, stamp, stamp_sz, id, id_sz) == 1;
}

int record_format(char *out, size_t out_sz, const char *stamp,
                  const char *id)
{
        int n = snprintf(out, out_sz, "%s %s", stamp, id);

        return n > 0 && (size_t)n < out_sz;
}

int era_get_xattr(int fd, char *state, size_t state_sz)
{
        char buf[ERA_STATE_MAX + sizeof RECORD_VERSION];
        size_t prefix = strlen(RECORD_VERSION);
        ssize_t n;

        n = fgetxattr(fd, ERA_XATTR, buf, sizeof buf - 1);
        if (n <= 0) {
                return -1;
        }
        buf[n] = '\0';
        /* Another version is treated like no record: copy it whole. */
        if (strncmp(buf, RECORD_VERSION, prefix) != 0) {
                return -1;
        }
        if ((size_t)n - prefix >= state_sz) {
                return -1;
        }
        memcpy(state, buf + prefix, (size_t)n - prefix + 1);
        return 0;
}

void record_store(int fd, const char *path, const char *state)
{
        char buf[ERA_STATE_MAX + sizeof RECORD_VERSION];
        int remove = !state || !state[0];
        int n;
        int rc;

        if (remove) {
                rc = fremovexattr(fd, ERA_XATTR);
        } else {
                n = snprintf(buf, sizeof buf, "%s%s", RECORD_VERSION, state);
                if (n < 0 || (size_t)n >= sizeof buf) {
                        die("era state for %s is too long", path);
                }
                rc = fsetxattr(fd, ERA_XATTR, buf, (size_t)n, 0);
        }
        if (rc == 0) {
                return;
        }
        if (remove && errno == ENODATA) {
                return;
        }

        die("cannot %s xattr %s on %s: %s%s",
            remove ? "remove" : "set", ERA_XATTR, path, strerror(errno),
            errno == ENOTSUP ? " (the destination filesystem needs "
            "user xattrs)" : "");
}

/* Second record in state, or NULL. */
static const char *dest_tail(const char *state)
{
        const char *p = state;
        const char *start;

        if (!state) {
                return NULL;
        }
        /* Skip past source record */
        if (record_parse(&p, NULL, NULL, 0, NULL, 0) != 1) {
                return NULL;
        }
        while (*p == ' ') {
                p++;
        }
        start = p;
        /* Verify that it is a valid record */
        if (record_parse(&p, NULL, NULL, 0, NULL, 0) != 1) {
                return NULL;
        }
        return start;
}

void era_set_source(int fd, const char *path, const char *source,
                    int old_fd)
{
        char old_xattr[ERA_STATE_MAX];
        char buf[ERA_STATE_MAX];
        const char *dest = NULL;
        int n;

        if (!source || !source[0]) {
                record_store(fd, path, "");
                return;
        }
        if (old_fd >= 0 &&
            era_get_xattr(old_fd, old_xattr, sizeof old_xattr) == 0) {
                dest = dest_tail(old_xattr);
        }
        if (!dest) {
                record_store(fd, path, source);
                return;
        }
        n = snprintf(buf, sizeof buf, "%s %s", source, dest);
        if (n < 0 || (size_t)n >= sizeof buf) {
                die("era state for %s is too long", path);
        }
        record_store(fd, path, buf);
}


int record_source_only(const char *state)
{
        const char *p = state;

        if (record_parse(&p, NULL, NULL, 0, NULL, 0) != 1) {
                return 0;
        }
        while (*p == ' ') {
                p++;
        }
        return *p == '\0';
}
