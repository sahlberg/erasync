/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "sender.h"

#include "common.h"
#include "era.h"
#include "fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int send_error(struct dest *d, const char *rel, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

/* Report an error found on the source side. */
static int send_error(struct dest *d, const char *rel, const char *fmt, ...)
{
        char msg[512];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        return d->ops->report(d->ctx, A_ERROR, rel, msg);
}

/* Source and destination have the same size and nothing is dirty. */
static void plan_unchanged(const struct stat *sst, const struct stat *dst,
                           int mtime_ok, struct file_plan *out)
{
        if (mtime_ok) {
                out->kind = same_meta(dst, sst, 0) ? PLAN_SKIP : PLAN_META;
                return;
        }

        /*
         * A write may have landed after the snapshot. Keep the destination
         * mtime different from the source, so the next run looks again.
         */
        out->set_mtime = 0;
        snprintf(out->why, sizeof out->why, "era window");
        out->kind = same_mode_owner(dst, sst, 0) ? PLAN_SKIP : PLAN_META;
}

void plan_file(int sfd, const struct stat *sst, int existed,
               const struct stat *dst, const char *xattr,
               const struct era_ranges *dest_dirty, struct file_plan *out)
{
        const char *why;
        int mtime_ok = 1;

        memset(out, 0, sizeof *out);
        out->set_mtime = 1;

        if (existed && dest_dirty->n == 0 && sst->st_size == dst->st_size &&
            times_equal(&sst->st_mtim, &dst->st_mtim)) {
                out->kind = same_meta(dst, sst, 0) ? PLAN_SKIP : PLAN_META;
                return;
        }

        if (existed && xattr && xattr[0]) {
                why = era_file_ranges(sfd, sst, xattr, &out->dirty,
                                      out->state, sizeof out->state,
                                      &mtime_ok);
                if (!why) {
                        if (out->dirty.n == 0 &&
                            dest_dirty->n == 0 &&
                            sst->st_size == dst->st_size) {
                                era_free_ranges(&out->dirty);
                                plan_unchanged(sst, dst, mtime_ok, out);
                                return;
                        }
                        if (dst->st_size < sst->st_size) {
                                era_add_range(&out->dirty, dst->st_size,
                                              sst->st_size - dst->st_size);
                        }
                        era_merge_ranges(&out->dirty, dest_dirty,
                                         sst->st_size);
                        out->kind = PLAN_RANGES;
                        out->set_mtime = mtime_ok;
                        return;
                }
                era_free_ranges(&out->dirty);
                snprintf(out->why, sizeof out->why, "%s", why);
        }

        era_state_for_copy(sfd, sst, out->state, sizeof out->state, &mtime_ok);
        out->kind = PLAN_FULL;
        out->set_mtime = mtime_ok;
}

/* A forced file is copied whole, whatever dm-era says. */
static void plan_forced(int sfd, const struct stat *sst,
                        struct file_plan *out)
{
        int mtime_ok = 1;

        memset(out, 0, sizeof *out);
        era_state_for_copy(sfd, sst, out->state, sizeof out->state, &mtime_ok);
        snprintf(out->why, sizeof out->why, "forced");
        out->kind = PLAN_FULL;
        out->set_mtime = mtime_ok;
}

/* A dry run decides from the size and mtime alone, without dm-era. */
static int report_dry_run(struct dest *d, const char *rel,
                          const struct stat *sst, const struct dest_state *ds,
                          int forced)
{
        if (!ds->exists) {
                return d->ops->report(d->ctx, A_CREATE, rel, "dry-run");
        }
        if (forced) {
                return d->ops->report(d->ctx, A_UPDATE, rel,
                                      "dry-run, forced");
        }
        if (ds->type == T_REG && sst->st_size == ds->st.st_size &&
            times_equal(&sst->st_mtim, &ds->st.st_mtim)) {
                return d->ops->report(d->ctx, A_SKIP, rel, NULL);
        }
        return d->ops->report(d->ctx, A_UPDATE, rel, "dry-run");
}

static int send_file(struct dest *d, int sdir, const char *sname,
                     const char *rel, const struct dest_state *ds, int forced)
{
        struct file_plan plan;
        struct put_source src;
        struct put_req req;
        struct stat sst;
        const char *why;
        int existed;
        int sfd;
        int rc;

        sfd = open_read(sdir, sname);
        if (sfd < 0) {
                return send_error(d, rel, "open source: %s", strerror(errno));
        }
        if (fstat(sfd, &sst) < 0 || !S_ISREG(sst.st_mode)) {
                rc = send_error(d, rel, "stat source: %s", strerror(errno));
                close(sfd);
                return rc;
        }
        why = era_device_problem(sst.st_dev);
        if (why) {
                rc = send_error(d, rel, "%s", why);
                close(sfd);
                return rc;
        }
        if (opt_dry_run) {
                rc = report_dry_run(d, rel, &sst, ds, forced);
                close(sfd);
                return rc;
        }

        existed = ds->exists && ds->type == T_REG && !ds->force_full;
        era_set_context(rel, (unsigned long long)sst.st_size);
        if (forced) {
                plan_forced(sfd, &sst, &plan);
        } else {
                plan_file(sfd, &sst, existed, &ds->st,
                          ds->xattr ? ds->xattr : "", &ds->dirty, &plan);
        }
        era_set_context(NULL, 0);

        switch (plan.kind) {
        case PLAN_SKIP:
                rc = d->ops->skip(d->ctx, rel, plan.why);
                break;
        case PLAN_META:
                rc = d->ops->meta(d->ctx, rel, &sst, plan.set_mtime);
                break;
        default:
                memset(&req, 0, sizeof req);
                req.rel = rel;
                req.full = plan.kind == PLAN_FULL;
                req.set_mtime = plan.set_mtime;
                req.st = sst;
                req.state = plan.state;
                req.why = req.full ? plan.why : "";
                src.fd = sfd;
                src.size = sst.st_size;
                src.ranges = req.full ? NULL : &plan.dirty;
                req.need = req.full ? (unsigned long long)sst.st_size
                                    : era_span_bytes(&plan.dirty);
                rc = d->ops->put(d->ctx, &req, &src);
                break;
        }
        era_free_ranges(&plan.dirty);
        close(sfd);
        return rc;
}

/* Send a file, symlink or special file. A forced file is copied whole. */
static int send_entry(struct dest *d, int dirfd, const char *name,
                      const char *rel, const struct stat *st,
                      const struct dest_state *ds, int forced)
{
        char *target;
        int rc;

        switch (file_type(st)) {
        case T_REG:
                return send_file(d, dirfd, name, rel, ds, forced);
        case T_LINK:
                target = read_link_at(dirfd, name);
                if (!target) {
                        return send_error(d, rel, "readlink: %s",
                                          strerror(errno));
                }
                rc = d->ops->link(d->ctx, rel, target, st);
                free(target);
                return rc;
        case T_SPECIAL:
                return d->ops->node(d->ctx, rel, st);
        default:
                return send_error(d, rel, "unsupported file type");
        }
}

static int send_dir(struct dest *d, int sfd, const char *rel,
                    const struct stat *self, int forced);

static int matches(const struct patterns *list, const char *rel,
                   const char *name, int type)
{
        char *path;
        int rc;

        if (!list->n) {
                return 0;
        }
        path = path_join(rel, name);
        rc = patterns_match(list, path, type == T_DIR);
        free(path);
        return rc;
}

static int send_child_dir(struct dest *d, int sfd, const char *name,
                          const char *rel, const struct stat *st, int forced)
{
        int fd;
        int rc;

        fd = open_dir(sfd, name);
        if (fd < 0) {
                return send_error(d, rel, "open directory: %s",
                                  strerror(errno));
        }
        rc = d->ops->dir(d->ctx, rel, st);
        if (rc == 0) {
                rc = send_dir(d, fd, rel, st, forced);
        }
        close(fd);
        return rc;
}

/*
 * Send the directory sfd: list its entries, so the receiver can delete
 * what is gone and return the state of each file, then send each entry.
 * Everything below a forced directory is forced.
 */
static int send_dir(struct dest *d, int sfd, const char *rel,
                    const struct stat *self, int forced)
{
        struct list_entry *entries;
        struct dest_state *states;
        struct stat *stats;
        struct names names;
        int avail;
        int rc = -1;
        size_t i;

        if (list_names(sfd, &names) < 0) {
                if (send_error(d, rel, "read directory: %s",
                               strerror(errno)) < 0) {
                        return -1;
                }
                return d->ops->dir_done(d->ctx, rel, NULL);
        }
        entries = xmalloc((names.n + 1) * sizeof *entries);
        stats = xmalloc((names.n + 1) * sizeof *stats);
        states = xmalloc((names.n + 1) * sizeof *states);
        for (i = 0; i < names.n; i++) {
                entries[i].name = names.v[i];
                entries[i].type = T_MISSING;
                if (fstatat(sfd, names.v[i], &stats[i],
                            AT_SYMLINK_NOFOLLOW) == 0) {
                        entries[i].type = file_type(&stats[i]);
                        if (matches(&opt_excludes, rel, names.v[i],
                                    entries[i].type)) {
                                /* Listed, so --delete keeps it, but skipped. */
                                entries[i].type = T_MISSING;
                        }
                } else if (errno != ENOENT) {
                        char *child = path_join(rel, names.v[i]);

                        rc = send_error(d, child, "stat: %s",
                                        strerror(errno));
                        free(child);
                        if (rc < 0) {
                                goto out;
                        }
                }
        }

        avail = d->ops->list(d->ctx, rel, 1, entries, names.n, states);
        if (avail < 0) {
                goto out;
        }
        for (i = 0; avail == 0 && i < names.n; i++) {
                int child_forced;
                char *child;

                if (erasync_stop) {
                        rc = -1;
                        goto out;
                }
                if (entries[i].type == T_MISSING) {
                        continue;
                }
                child = path_join(rel, names.v[i]);
                child_forced = forced || matches(&opt_forces, rel, names.v[i],
                                                 entries[i].type);
                if (entries[i].type == T_DIR) {
                        rc = send_child_dir(d, sfd, names.v[i], child,
                                            &stats[i], child_forced);
                } else {
                        rc = send_entry(d, sfd, names.v[i], child, &stats[i],
                                        &states[i], child_forced);
                }
                free(child);
                if (rc < 0) {
                        goto out;
                }
        }
        rc = d->ops->dir_done(d->ctx, rel, avail == 0 ? self : NULL);

out:
        for (i = 0; i < names.n; i++) {
                dest_state_free(&states[i]);
        }
        free(states);
        free(stats);
        free(entries);
        names_free(&names);
        return rc;
}

static int send_tree(struct dest *d, const char *src)
{
        struct stat st;
        int sfd;
        int rc;

        sfd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sfd < 0) {
                send_error(d, src, "open: %s", strerror(errno));
                return -1;
        }
        if (fstat(sfd, &st) < 0) {
                send_error(d, src, "stat: %s", strerror(errno));
                close(sfd);
                return -1;
        }
        rc = send_dir(d, sfd, "", &st, opt_force_all);
        close(sfd);
        return rc;
}

/* Send the file name in parent to rel, its path at the destination. */
static int send_single(struct dest *d, const char *parent, const char *name,
                       const char *rel)
{
        struct list_entry entry;
        struct dest_state ds;
        struct stat st;
        int sdir;
        int rc = -1;

        sdir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sdir < 0) {
                send_error(d, parent, "open: %s", strerror(errno));
                return -1;
        }
        if (fstatat(sdir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                send_error(d, name, "stat: %s", strerror(errno));
                goto out;
        }

        entry.name = rel;
        entry.type = file_type(&st);
        memset(&ds, 0, sizeof ds);
        if (d->ops->list(d->ctx, "", 0, &entry, 1, &ds) == 0) {
                rc = send_entry(d, sdir, name, rel, &st, &ds,
                                opt_force_all ||
                                matches(&opt_forces, "", name, entry.type));
        }
        dest_state_free(&ds);

out:
        close(sdir);
        return rc;
}

int send_all(struct dest *d, const char *src)
{
        struct stat st;
        struct stat dev_st;
        const char *why;
        char *parent = NULL;
        char *name = NULL;
        char *rel = NULL;
        int src_is_dir;
        int rc = -1;

        if (lstat(src, &st) < 0) {
                send_error(d, src, "%s", strerror(errno));
                return -1;
        }
        /* The filesystem the source is on, through a symlink if need be. */
        if (stat(src, &dev_st) < 0) {
                dev_st = st;
        }
        why = era_device_problem(dev_st.st_dev);
        if (why) {
                send_error(d, src, "%s", why);
                return -1;
        }

        src_is_dir = lstat_is_dir(src, &st);
        if (!src_is_dir) {
                if (file_type(&st) == T_OTHER) {
                        send_error(d, src, "not a file or directory");
                        return -1;
                }
                split_path(src, &parent, &name);
                if (bad_name(name)) {
                        send_error(d, src, "not a file");
                        goto out;
                }
        }

        if (d->ops->root(d->ctx, src_is_dir, name ? name : "", &rel) < 0) {
                goto out;
        }
        if (src_is_dir) {
                rc = send_tree(d, src);
        } else {
                rc = send_single(d, parent, name, rel);
        }
        if (rc == 0) {
                rc = d->ops->done(d->ctx);
        }

out:
        free(rel);
        free(name);
        free(parent);
        return rc;
}
