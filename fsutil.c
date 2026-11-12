/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "fsutil.h"

#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/falloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ZERO_BUF_SIZE = 1024 * 1024 };

int is_dot(const char *name)
{
        return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* A fifo, socket or device node. */
int is_special_file(mode_t mode)
{
        return S_ISFIFO(mode) || S_ISSOCK(mode) || S_ISBLK(mode) ||
                S_ISCHR(mode);
}

int file_type(const struct stat *st)
{
        if (S_ISDIR(st->st_mode)) {
                return T_DIR;
        }
        if (S_ISREG(st->st_mode)) {
                return T_REG;
        }
        if (S_ISLNK(st->st_mode)) {
                return T_LINK;
        }
        if (is_special_file(st->st_mode)) {
                return T_SPECIAL;
        }
        return T_OTHER;
}

int is_temp_name(const char *name)
{
        return strncmp(name, TEMP_PREFIX, strlen(TEMP_PREFIX)) == 0;
}

int ends_with_slash(const char *path)
{
        size_t n = strlen(path);

        return n > 0 && path[n - 1] == '/';
}

int is_dir_follow(const char *path)
{
        struct stat st;

        if (stat(path, &st) < 0) {
                return 0;
        }
        return S_ISDIR(st.st_mode);
}

int lstat_is_dir(const char *path, const struct stat *st)
{
        if (S_ISDIR(st->st_mode)) {
                return 1;
        }
        return S_ISLNK(st->st_mode) && is_dir_follow(path);
}

void split_path(const char *path, char **parent, char **name)
{
        size_t len = strlen(path);
        const char *slash = NULL;
        size_t i;

        while (len > 1 && path[len - 1] == '/') {
                len--;
        }
        for (i = 0; i < len; i++) {
                if (path[i] == '/') {
                        slash = path + i;
                }
        }

        if (!slash) {
                *parent = xstrdup(".");
                *name = strndup(path, len);
        } else if (slash == path) {
                *parent = xstrdup("/");
                *name = strndup(path + 1, len - 1);
        } else {
                *parent = strndup(path, slash - path);
                *name = strndup(slash + 1, len - (slash + 1 - path));
        }
        if (!*parent || !*name) {
                die("out of memory");
        }
}

char *path_join(const char *dir, const char *rel)
{
        size_t len;
        char *p;

        if (!rel || !rel[0]) {
                return xstrdup(dir ? dir : "");
        }
        if (!dir || !dir[0]) {
                return xstrdup(rel);
        }
        len = strlen(dir);
        p = xmalloc(len + strlen(rel) + 2);
        if (dir[len - 1] == '/') {
                sprintf(p, "%s%s", dir, rel);
        } else {
                sprintf(p, "%s/%s", dir, rel);
        }
        return p;
}

int bad_name(const char *name)
{
        return !name[0] || is_dot(name) || strchr(name, '/') != NULL;
}

int bad_rel(const char *rel)
{
        const char *s = rel;
        const char *slash;
        size_t n;

        if (!rel || rel[0] == '/') {
                return 1;
        }
        if (!rel[0]) {
                return 0;
        }
        for (;;) {
                slash = strchrnul(s, '/');
                n = (size_t)(slash - s);
                if (n == 0 || n > NAME_MAX) {
                        return 1;
                }
                if (n == 1 && s[0] == '.') {
                        return 1;
                }
                if (n == 2 && s[0] == '.' && s[1] == '.') {
                        return 1;
                }
                if (!*slash) {
                        return 0;
                }
                s = slash + 1;
        }
}

static int open_noatime(int dirfd, const char *name, int flags)
{
        int fd;

        fd = openat(dirfd, name, flags | O_NOATIME);
        if (fd < 0 && (errno == EPERM || errno == EINVAL)) {
                fd = openat(dirfd, name, flags);
        }
        return fd;
}

int open_read(int dirfd, const char *name)
{
        return open_noatime(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
}

int open_dir(int dirfd, const char *name)
{
        return open_noatime(dirfd, name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Open the directory fd for reading entries, without consuming fd. */
static DIR *open_entries(int fd)
{
        int dupfd;
        DIR *d;

        dupfd = dup(fd);
        if (dupfd < 0) {
                return NULL;
        }
        d = fdopendir(dupfd);
        if (!d) {
                close(dupfd);
        }
        return d;
}

int remove_tree(int dirfd, const char *name)
{
        struct dirent *de;
        struct stat st;
        DIR *d;
        int fd;
        int rc = 0;

        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                return errno == ENOENT ? 0 : -1;
        }
        if (!S_ISDIR(st.st_mode)) {
                if (unlinkat(dirfd, name, 0) < 0 && errno != ENOENT) {
                        return -1;
                }
                return 0;
        }

        fd = openat(dirfd, name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
                return -1;
        }
        d = open_entries(fd);
        if (!d) {
                close(fd);
                return -1;
        }
        while ((de = readdir(d)) != NULL) {
                if (is_dot(de->d_name)) {
                        continue;
                }
                if (remove_tree(fd, de->d_name) < 0) {
                        rc = -1;
                        break;
                }
        }
        closedir(d);
        close(fd);
        if (rc < 0) {
                return -1;
        }

        if (unlinkat(dirfd, name, AT_REMOVEDIR) < 0 && errno != ENOENT) {
                return -1;
        }
        return 0;
}

char *read_link_at(int dirfd, const char *name)
{
        size_t size = 64;
        char *buf;
        ssize_t n;

        for (;;) {
                buf = xmalloc(size);
                n = readlinkat(dirfd, name, buf, size);
                if (n < 0) {
                        int saved_errno = errno;

                        free(buf);
                        errno = saved_errno;
                        return NULL;
                }
                if ((size_t)n < size) {
                        buf[n] = '\0';
                        return buf;
                }
                free(buf);
                if (size > (1 << 20)) {
                        errno = ENAMETOOLONG;
                        return NULL;
                }
                size *= 2;
        }
}

static int cmp_names(const void *a, const void *b)
{
        return strcmp(*(char * const *)a, *(char * const *)b);
}

void names_add(struct names *ns, const char *name)
{
        if (ns->n == ns->cap) {
                ns->cap = ns->cap ? ns->cap * 2 : 32;
                ns->v = xrealloc(ns->v, ns->cap * sizeof *ns->v);
        }
        ns->v[ns->n++] = xstrdup(name);
}

void names_sort(struct names *ns)
{
        if (ns->n) {
                qsort(ns->v, ns->n, sizeof *ns->v, cmp_names);
        }
}

int names_has(const struct names *ns, const char *name)
{
        if (ns->n == 0) {
                return 0;
        }
        return bsearch(&name, ns->v, ns->n, sizeof *ns->v, cmp_names) != NULL;
}

void names_free(struct names *ns)
{
        size_t i;

        for (i = 0; i < ns->n; i++) {
                free(ns->v[i]);
        }
        free(ns->v);
        memset(ns, 0, sizeof *ns);
}

int list_names(int fd, struct names *ns)
{
        struct dirent *de;
        DIR *d;

        memset(ns, 0, sizeof *ns);
        d = open_entries(fd);
        if (!d) {
                return -1;
        }
        while ((de = readdir(d)) != NULL) {
                if (!is_dot(de->d_name)) {
                        names_add(ns, de->d_name);
                }
        }
        closedir(d);
        names_sort(ns);
        return 0;
}

int pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
        const char *p = buf;
        ssize_t n;

        while (len > 0) {
                n = pwrite(fd, p, len, off);
                if (n < 0 && errno == EINTR) {
                        continue;
                }
                if (n < 0) {
                        return -1;
                }
                if (n == 0) {
                        errno = EIO;
                        return -1;
                }
                p += n;
                off += n;
                len -= (size_t)n;
        }
        return 0;
}

int punch_hole(int fd, off_t off, off_t len, off_t *zeroed)
{
        static char *zeros;

        if (len <= 0) {
                return 0;
        }
        if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      off, len) == 0) {
                return 0;
        }
        if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EINVAL) {
                return -1;
        }

        if (!zeros) {
                zeros = xmalloc(ZERO_BUF_SIZE);
        }
        while (len > 0) {
                size_t n = len > ZERO_BUF_SIZE ? ZERO_BUF_SIZE : (size_t)len;

                if (pwrite_full(fd, zeros, n, off) < 0) {
                        return -1;
                }
                off += n;
                len -= n;
                *zeroed += n;
        }
        return 0;
}

int times_equal(const struct timespec *a, const struct timespec *b)
{
        return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

int same_mode_owner(const struct stat *dst, const struct stat *src,
                    int symlink)
{
        if (!symlink && (dst->st_mode & 07777) != (src->st_mode & 07777)) {
                return 0;
        }
        if (geteuid() != 0) {
                return 1;
        }
        return dst->st_uid == src->st_uid && dst->st_gid == src->st_gid;
}

int same_special(const struct stat *a, const struct stat *b)
{
        if ((a->st_mode & S_IFMT) != (b->st_mode & S_IFMT)) {
                return 0;
        }
        if (!S_ISBLK(a->st_mode) && !S_ISCHR(a->st_mode)) {
                return 1;
        }
        return a->st_rdev == b->st_rdev;
}

int same_meta(const struct stat *dst, const struct stat *src, int symlink)
{
        if (!same_mode_owner(dst, src, symlink)) {
                return 0;
        }
        return times_equal(&dst->st_mtim, &src->st_mtim);
}

static void mtime_only(struct timespec ts[2], const struct stat *src)
{
        ts[0].tv_sec = 0;
        ts[0].tv_nsec = UTIME_OMIT;
        ts[1] = src->st_mtim;
}

int apply_meta_fd(int fd, const struct stat *src, int set_mtime)
{
        struct timespec ts[2];

        if (fchmod(fd, src->st_mode & 07777) < 0) {
                return -1;
        }
        if (geteuid() == 0 && fchown(fd, src->st_uid, src->st_gid) < 0 &&
            errno != EPERM) {
                return -1;
        }
        if (!set_mtime) {
                return 0;
        }
        mtime_only(ts, src);
        return futimens(fd, ts);
}

int apply_owner_time_at(int dirfd, const char *name, const struct stat *src)
{
        struct timespec ts[2];

        if (geteuid() == 0 &&
            fchownat(dirfd, name, src->st_uid, src->st_gid,
                     AT_SYMLINK_NOFOLLOW) < 0 &&
            errno != EPERM) {
                return -1;
        }
        mtime_only(ts, src);
        return utimensat(dirfd, name, ts, AT_SYMLINK_NOFOLLOW);
}

int apply_meta_at(int dirfd, const char *name, const struct stat *src)
{
        if (fchmodat(dirfd, name, src->st_mode & 07777, 0) < 0) {
                return -1;
        }
        return apply_owner_time_at(dirfd, name, src);
}
