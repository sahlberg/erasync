/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#ifndef ERASYNC_CONFIG_H
#define ERASYNC_CONFIG_H

/*
 * $XDG_CONFIG_HOME/erasync/config, or ~/.config/erasync/config. ~ comes
 * from the password file when HOME is unset.
 */
char *config_default_path(void);

/*
 * The DEST of the sync lines in the config file at path whose SRC is src,
 * as written there. It is an error when there is none, or more than one.
 */
char *config_dest(const char *path, const char *src);

/*
 * The options in the config file at path that apply to a copy of src to
 * dst, as an argv with the path as argv[0] and one option per element.
 * A missing file gives no options, unless must_exist is set. Errors in
 * the file are fatal.
 */
void config_read(const char *path, int must_exist, const char *src,
                 const char *dst, int *argc, char ***argv);

#endif
