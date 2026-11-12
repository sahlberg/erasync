/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_FSUTIL_H
#define ERASYNC_FSUTIL_H

/* File and directory helpers shared by the local and the remote sync. */

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Unfinished copies in a destination directory start with this. */
#define TEMP_PREFIX ".erasync.tmp."

/* A sorted list of directory entry names. */
struct names {
        char **v;
        size_t n;
        size_t cap;
};

/* Entry types, as sent in the protocol. */
enum {
        T_MISSING = 0,
        T_DIR,
        T_REG,
        T_LINK,
        T_SPECIAL,
        T_OTHER,
};

int file_type(const struct stat *st);

int is_dot(const char *name);
int is_special_file(mode_t mode);
int is_temp_name(const char *name);
int ends_with_slash(const char *path);

/* Whether path is a directory, following symlinks. */
int is_dir_follow(const char *path);

/* st is the lstat of path. Whether path is a directory or a link to one. */
int lstat_is_dir(const char *path, const struct stat *st);

/*
 * Split path into its parent directory and its final component.
 * Trailing slashes are ignored. The parent is "." when path has no slash.
 */
void split_path(const char *path, char **parent, char **name);

/* dir/rel. Either may be empty. */
char *path_join(const char *dir, const char *rel);

/* A name that cannot be a directory entry: "", ".", ".." or with a '/'. */
int bad_name(const char *name);

/* A relative path that could leave its base directory. "" is fine. */
int bad_rel(const char *rel);

/* Open for reading without following symlinks, with O_NOATIME if allowed. */
int open_read(int dirfd, const char *name);
int open_dir(int dirfd, const char *name);

/* Remove name, and everything below it when it is a directory. */
int remove_tree(int dirfd, const char *name);

/* The target of a symlink, or NULL with errno set. */
char *read_link_at(int dirfd, const char *name);

void names_add(struct names *ns, const char *name);
void names_sort(struct names *ns);
int names_has(const struct names *ns, const char *name);
void names_free(struct names *ns);

/* All entries of the directory fd except "." and "..", sorted. */
int list_names(int fd, struct names *ns);

int pwrite_full(int fd, const void *buf, size_t len, off_t off);

/*
 * Punch a hole. When the filesystem cannot punch, write zeros instead and
 * add the number of bytes written to *zeroed.
 */
int punch_hole(int fd, off_t off, off_t len, off_t *zeroed);

int times_equal(const struct timespec *a, const struct timespec *b);

/* Mode and owner match. The mode of a symlink is not compared. */
int same_mode_owner(const struct stat *dst, const struct stat *src,
                    int symlink);

/* Same kind of special file, and the same device for a device node. */
int same_special(const struct stat *a, const struct stat *b);

/* Mode, owner and mtime match. */
int same_meta(const struct stat *dst, const struct stat *src, int symlink);

/*
 * Apply the mode, owner and, when set_mtime is set, the mtime of src.
 * The owner is only changed when running as root.
 */
int apply_meta_fd(int fd, const struct stat *src, int set_mtime);

/* Apply the owner and mtime of src to name, without following symlinks. */
int apply_owner_time_at(int dirfd, const char *name, const struct stat *src);

/*
 * Apply the mode, owner and mtime of src to name. Used for special files,
 * which are not opened: opening a fifo for reading blocks.
 */
int apply_meta_at(int dirfd, const char *name, const struct stat *src);

#endif
