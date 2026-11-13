/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
/*
 * The config file holds options so they need not be typed on every run.
 * Options before the first "sync" line apply to every copy. A line
 * "sync SRC DEST" starts options that apply only to a copy of that SRC
 * to that DEST, up to the next sync line. Local names are compared as
 * absolute paths with symlinks resolved, host:path names as written:
 *
 *     --verbose
 *
 *     sync /vm/images 10.10.10.11:/backup/images
 *             --exclude *-tester*.qcow2
 *             --delete
 *
 * One option per line, as on the command line but without shell quoting.
 * An option and its argument are separated by '=' or by spaces. Blank
 * lines and lines starting with '#' are ignored. The options are parsed
 * before those of the command line, which may add to them.
 *
 * When only SRC is given, DEST comes from the sync lines for that SRC,
 * if they all name the same DEST.
 */
#include "config.h"
#include "common.h"
#include "remote.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *config_default_path(void)
{
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        struct passwd *pw;
        char *path;

        /* The XDG spec ignores a relative XDG_CONFIG_HOME. */
        if (xdg && *xdg == '/') {
                if (asprintf(&path, "%s/erasync/config", xdg) < 0) {
                        die("out of memory");
                }
                return path;
        }
        if (!home || !*home) {
                pw = getpwuid(getuid());
                if (!pw) {
                        die("cannot find the home directory");
                }
                home = pw->pw_dir;
        }
        if (asprintf(&path, "%s/.config/erasync/config", home) < 0) {
                die("out of memory");
        }
        return path;
}

static char *trim(char *s)
{
        char *end;

        while (isspace((unsigned char)*s)) {
                s++;
        }
        end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) {
                end--;
        }
        *end = '\0';
        return s;
}

/*
 * The name to compare: a local path made absolute, with symlinks resolved
 * when it exists, and host:path as it is. Trailing slashes are dropped,
 * except for "/" and "host:/".
 */
static char *canonical(const char *spec)
{
        char *host = NULL;
        char *path = NULL;
        char *name;
        char *cwd;
        size_t n;

        if (remote_split(spec, &host, &path)) {
                name = xstrdup(spec);
        } else {
                name = realpath(spec, NULL);
        }
        free(host);
        free(path);
        if (!name && spec[0] == '/') {
                name = xstrdup(spec);
        }
        if (!name) {
                cwd = getcwd(NULL, 0);
                if (!cwd) {
                        die("getcwd: %s", strerror(errno));
                }
                if (asprintf(&name, "%s/%s", cwd, spec) < 0) {
                        die("out of memory");
                }
                free(cwd);
        }
        n = strlen(name);
        while (n > 1 && name[n - 1] == '/' && name[n - 2] != ':') {
                name[--n] = '\0';
        }
        return name;
}

static FILE *open_config(const char *path, int must_exist)
{
        FILE *fp = fopen(path, "r");

        if (!fp && (errno != ENOENT || must_exist)) {
                die("%s: %s", path, strerror(errno));
        }
        return fp;
}

/*
 * The next line that is not blank or a comment, trimmed, or NULL at the
 * end of the file.
 */
static char *next_line(FILE *fp, const char *path, char **line, size_t *cap,
                       unsigned *lineno)
{
        char *s;

        while (getline(line, cap, fp) != -1) {
                (*lineno)++;
                s = trim(*line);
                if (!*s || *s == '#') {
                        continue;
                }
                if (strpbrk(s, "'\"")) {
                        die("%s:%u: no quotes, write the value as it is",
                            path, *lineno);
                }
                return s;
        }
        if (ferror(fp)) {
                die("%s: %s", path, strerror(errno));
        }
        return NULL;
}

static int is_sync(const char *s)
{
        return !strncmp(s, "sync", 4) && isspace((unsigned char)s[4]);
}

/* The SRC and DEST of the sync line s, as written. */
static void sync_names(const char *path, unsigned lineno, char *s,
                       char **src, char **dst)
{
        *src = strtok(s + 4, " \t");
        *dst = strtok(NULL, " \t");
        if (!*dst || strtok(NULL, " \t")) {
                die("%s:%u: expected sync SRC DEST", path, lineno);
        }
}

/* Whether name, as written in the config, is the canonical name. */
static int same_name(const char *name, const char *canon)
{
        char *c = canonical(name);
        int same = !strcmp(c, canon);

        free(c);
        return same;
}

/* One argv element: "--opt ARG" as "--opt=ARG", and "-o ARG" as "-oARG". */
static char *option_word(char *line)
{
        size_t len = strcspn(line, " \t=");
        char *joined;
        char *arg;

        if (line[len] != ' ' && line[len] != '\t') {
                return xstrdup(line);
        }
        line[len] = '\0';
        arg = trim(line + len + 1);
        if (asprintf(&joined, "%s%s%s", line, line[1] == '-' ? "=" : "",
                     arg) < 0) {
                die("out of memory");
        }
        return joined;
}

char *config_dest(const char *path, const char *src)
{
        FILE *fp;
        char *line = NULL;
        size_t cap = 0;
        unsigned lineno = 0;
        unsigned found_line = 0;
        char *found = NULL;
        char *found_name = NULL;
        char *src_name;
        char *cfg_src;
        char *cfg_dst;
        char *s;

        fp = fopen(path, "r");
        if (!fp) {
                die("no DEST given, and %s: %s", path, strerror(errno));
        }
        src_name = canonical(src);
        while ((s = next_line(fp, path, &line, &cap, &lineno))) {
                if (!is_sync(s)) {
                        continue;
                }
                sync_names(path, lineno, s, &cfg_src, &cfg_dst);
                if (!same_name(cfg_src, src_name)) {
                        continue;
                }
                if (!found) {
                        found = xstrdup(cfg_dst);
                        found_name = canonical(cfg_dst);
                        found_line = lineno;
                        continue;
                }
                if (!same_name(cfg_dst, found_name)) {
                        die("no DEST given, and %s has more than one DEST for "
                            "%s, on lines %u and %u", path, src,
                            found_line, lineno);
                }
        }
        if (!found) {
                die("no DEST given, and %s has no sync line for %s",
                    path, src);
        }
        free(found_name);
        free(src_name);
        free(line);
        fclose(fp);
        return found;
}

void config_read(const char *path, int must_exist, const char *src,
                 const char *dst, int *argc, char ***argv)
{
        FILE *fp;
        char *line = NULL;
        size_t cap = 0;
        unsigned lineno = 0;
        int active = 1;
        char *src_name;
        char *dst_name;
        char *cfg_src;
        char *cfg_dst;
        char *s;

        *argc = 1;
        *argv = xmalloc(2 * sizeof **argv);
        (*argv)[0] = xstrdup(path);

        fp = open_config(path, must_exist);
        if (!fp) {
                return;
        }
        src_name = canonical(src);
        dst_name = canonical(dst);
        while ((s = next_line(fp, path, &line, &cap, &lineno))) {
                if (is_sync(s)) {
                        sync_names(path, lineno, s, &cfg_src, &cfg_dst);
                        active = same_name(cfg_src, src_name) &&
                                 same_name(cfg_dst, dst_name);
                        continue;
                }
                if (*s != '-' || !strcmp(s, "--")) {
                        die("%s:%u: expected an option or a sync line",
                            path, lineno);
                }
                if (!active) {
                        continue;
                }
                *argv = xrealloc(*argv, (*argc + 2) * sizeof **argv);
                (*argv)[(*argc)++] = option_word(s);
                (*argv)[*argc] = NULL;
        }
        free(src_name);
        free(dst_name);
        free(line);
        fclose(fp);
}
