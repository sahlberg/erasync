/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/* Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause */
#include "common.h"
#include "config.h"
#include "era.h"
#include "remote.h"
#include "sync.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char help_text[] =
        "Usage: erasync [options] SRC [DEST]\n"
        "\n"
        "Make DEST a copy of the directory tree SRC. SRC may also be a single\n"
        "file, symlink, or special file: it is written to DEST, or into DEST\n"
        "when DEST is a directory. Files, directories, symlinks, fifos,\n"
        "sockets, and device nodes are replicated. Device nodes are recreated.\n"
        "A hard link is copied as its own file. Entries already in DEST that\n"
        "are not in SRC are left in place unless --delete is given. --delete\n"
        "applies to a directory tree. Symlinks are copied as symlinks and are\n"
        "not followed.\n"
        "\n"
        "A regular file on a dm-era device is updated from the blocks written\n"
        "since the era stored on the destination, instead of being copied\n"
        "whole. That era is kept in the user.erasync.era extended attribute\n"
        "and in /var/lib/erasync/era/<device-id>. The first copy of a file is\n"
        "the whole file. Later copies read and write only blocks dm-era has\n"
        "seen change, plus blocks written on a destination that is itself on\n"
        "dm-era. A matching size and mtime skips that check, except when the\n"
        "destination era shows a later write.\n"
        "\n"
        "SRC and DEST must both be on dm-era devices; use cp or rsync for\n"
        "anything else. erasync must run as root, and needs era_invalidate\n"
        "from thin-provisioning-tools and user extended attributes on DEST.\n"
        "While a file is copied the line is bytes sent / bytes this copy\n"
        "will read / size of the whole file.\n"
        "\n"
        "Either SRC or DEST may be [USER@]HOST:PATH. erasync runs\n"
        "erasync --server on that host over ssh and copies through the\n"
        "connection. One side stays local. A path with a slash before the colon\n"
        "is a local name.\n"
        "\n"
        "Options:\n"
        "  -n, --dry-run   show actions without writing or taking era snapshots\n"
        "  -v, --verbose   show unchanged files and how a file was copied\n"
        "  -q, --quiet     errors only, and no copy progress\n"
        "      --delete    remove DEST entries that are not in SRC\n"
        "                  (the walk does not follow symlinks)\n"
        "      --exclude PATTERN\n"
        "                  skip entries that match PATTERN, and keep them in\n"
        "                  DEST with --delete. May be given more than once.\n"
        "      --force[=PATTERN]\n"
        "                  copy files whole instead of the changed blocks:\n"
        "                  all of them, or those that match PATTERN. Note the\n"
        "                  '='. May be given more than once.\n"
        "  -e, --rsh CMD   remote shell to run instead of ssh\n"
        "      --remote-path PROG\n"
        "                  erasync on the remote host (default: erasync)\n"
        "      --server    speak the copy protocol on standard input\n"
        "      --config FILE\n"
        "                  read options from FILE instead of\n"
        "                  ~/.config/erasync/config\n"
        "      --no-config don't read a config file\n"
        "  -h, --help\n"
        "\n"
        "A PATTERN without a '/' matches an entry's name at any depth, such as\n"
        "--exclude='*-tester*.qcow2'. One with a '/' matches the end of the\n"
        "path below SRC, unless a leading '/' anchors it at the top of SRC.\n"
        "A trailing '/' matches directories only, and a forced directory\n"
        "forces everything below it.\n"
        "\n"
        "The config file holds options to use without typing them. Options\n"
        "before the first \"sync SRC DEST\" line apply to every copy, the\n"
        "ones after it only to a copy of that SRC to that DEST. One option per\n"
        "line, without shell quotes; see erasync(1). For example:\n"
        "\n"
        "  sync /vm/images 10.10.10.11:/backup/images\n"
        "          --exclude *-tester*.qcow2\n"
        "\n"
        "Without DEST, the DEST of the sync line for SRC is used.\n"
        "\n"
        "Names starting with .erasync.tmp. in DEST are unfinished copies and\n"
        "are removed. Deleting an era log makes the next sync copy those\n"
        "files whole once.\n";

enum {
        OPT_DELETE = 256,
        OPT_SERVER,
        OPT_REMOTE_PATH,
        OPT_EXCLUDE,
        OPT_FORCE,
        OPT_CONFIG,
        OPT_NO_CONFIG,
};

static const struct option long_options[] = {
        { "help",        no_argument,       NULL, 'h' },
        { "verbose",     no_argument,       NULL, 'v' },
        { "quiet",       no_argument,       NULL, 'q' },
        { "dry-run",     no_argument,       NULL, 'n' },
        { "rsh",         required_argument, NULL, 'e' },
        { "delete",      no_argument,       NULL, OPT_DELETE },
        { "server",      no_argument,       NULL, OPT_SERVER },
        { "remote-path", required_argument, NULL, OPT_REMOTE_PATH },
        { "exclude",     required_argument, NULL, OPT_EXCLUDE },
        { "force",       optional_argument, NULL, OPT_FORCE },
        { "config",      required_argument, NULL, OPT_CONFIG },
        { "no-config",   no_argument,       NULL, OPT_NO_CONFIG },
        { NULL, 0, NULL, 0 }
};

static const char short_options[] = "+:hvqne:";

/* --config and --no-config, found by scan_options. */
static const char *config_path;
static int no_config;

/* The options taken from the config file, shown unless -q. */
static int config_argc;
static char **config_argv;
/* DEST was taken from the config file. */
static int config_dest_used;

static void usage(int code)
{
        fputs(help_text, code ? stderr : stdout);
        exit(code);
}

static void on_signal(int sig)
{
        (void)sig;
        erasync_stop = 1;
}

static void summary(void)
{
        char size[32];

        if (opt_quiet) {
                return;
        }
        printf("erasync:%s created %lu, updated %lu, metadata %lu, "
               "unchanged %lu, deleted %lu, errors %lu, ",
               opt_dry_run ? " dry-run:" : "",
               stats.created, stats.updated, stats.meta, stats.unchanged,
               stats.deleted, stats.errors);
        if (opt_dry_run && stats.estimate) {
                format_bytes(size, sizeof size, stats.estimate);
                printf("up to %s to copy%s\n", size, ESTIMATE);
                return;
        }
        printf("copied %llu bytes\n", (unsigned long long)stats.bytes);
}

/*
 * Parse argv, the command line or the options from the config file
 * (in_config, with the file name as argv[0]). -v and -q override each
 * other between the two. Returns whether --server was given.
 */
static int parse_options(int argc, char **argv, int in_config)
{
        const char *where = in_config ? argv[0] : "";
        const char *sep = in_config ? ": " : "";
        int server = 0;
        int verbose = 0;
        int quiet = 0;
        int c;

        opterr = 0;
        optind = 0;
        /* '+' stops at the first operand, ':' reports a missing argument */
        while ((c = getopt_long(argc, argv, short_options, long_options,
                                NULL)) != -1) {
                if (in_config && (c == 'h' || c == OPT_SERVER ||
                                  c == OPT_CONFIG || c == OPT_NO_CONFIG)) {
                        die("%s: %s is not allowed in the config file",
                            where, argv[optind - 1]);
                }
                switch (c) {
                case 'h':
                        usage(0);
                        break;
                case 'v':
                        verbose = 1;
                        break;
                case 'q':
                        quiet = 1;
                        break;
                case 'n':
                        opt_dry_run = 1;
                        break;
                case 'e':
                        remote_set_rsh(optarg);
                        break;
                case OPT_DELETE:
                        opt_delete = 1;
                        break;
                case OPT_SERVER:
                        server = 1;
                        break;
                case OPT_REMOTE_PATH:
                        remote_set_program(optarg);
                        break;
                case OPT_EXCLUDE:
                        patterns_add(&opt_excludes, optarg);
                        break;
                case OPT_FORCE:
                        if (optarg) {
                                patterns_add(&opt_forces, optarg);
                        } else {
                                opt_force_all = 1;
                        }
                        break;
                case OPT_CONFIG:
                case OPT_NO_CONFIG:
                        /* taken by scan_options */
                        break;
                case ':':
                        die("%s%s%s needs an argument", where, sep,
                            argv[optind - 1]);
                default:
                        if (optopt) {
                                die("%s%sunknown option -%c", where, sep,
                                    optopt);
                        }
                        die("%s%sunknown option %s", where, sep,
                            argv[optind - 1]);
                }
        }
        if (in_config && optind < argc) {
                die("%s: unexpected %s", where, argv[optind]);
        }
        if (verbose && quiet) {
                die("%s%sdo not combine -v and -q", where, sep);
        }
        if (verbose) {
                opt_verbose = 1;
                opt_quiet = 0;
        }
        if (quiet) {
                opt_quiet = 1;
                opt_verbose = 0;
        }
        return server;
}

/*
 * Find --server, --config and --no-config, and leave optind at the
 * operands, before the config file is read. -h and bad options are
 * handled by parse_options, before the config file can fail.
 */
static int scan_options(int argc, char **argv)
{
        int server = 0;
        int c;

        opterr = 0;
        optind = 0;
        while ((c = getopt_long(argc, argv, short_options, long_options,
                                NULL)) != -1) {
                switch (c) {
                case OPT_SERVER:
                        server = 1;
                        break;
                case OPT_CONFIG:
                        config_path = optarg;
                        break;
                case OPT_NO_CONFIG:
                        no_config = 1;
                        break;
                case 'h':
                case ':':
                case '?':
                        parse_options(argc, argv, 0);
                        break;
                }
        }
        return server;
}

/* The config file to read, or NULL with --no-config. */
static char *config_file(void)
{
        if (no_config) {
                return NULL;
        }
        if (config_path) {
                return xstrdup(config_path);
        }
        return config_default_path();
}

/* DEST from the sync line for src. */
static char *find_dest(const char *src)
{
        char *path = config_file();
        char *dst;

        if (!path) {
                die("no DEST given, and --no-config");
        }
        dst = config_dest(path, src);
        free(path);
        config_dest_used = 1;
        return dst;
}

/* Parse the options from the config file for a copy of src to dst. */
static void read_config(const char *src, const char *dst)
{
        char *path = config_file();

        if (!path) {
                return;
        }
        config_read(path, config_path != NULL, src, dst, &config_argc,
                    &config_argv);
        free(path);
        parse_options(config_argc, config_argv, 1);
}

static void show_config(const char *dst)
{
        int i;

        if (opt_quiet || !config_argv) {
                return;
        }
        if (config_dest_used) {
                printf("erasync: DEST %s from %s\n", dst, config_argv[0]);
        }
        if (config_argc < 2) {
                return;
        }
        printf("erasync: options from %s:", config_argv[0]);
        for (i = 1; i < config_argc; i++) {
                printf(" %s", config_argv[i]);
        }
        printf("\n");
}

static void catch_signals(void)
{
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
}

static int run_server(void)
{
        return remote_server() < 0 ? 2 : 0;
}

static int run_sync(const char *src, const char *dst)
{
        char *src_host = NULL;
        char *src_path = NULL;
        char *dst_host = NULL;
        char *dst_path = NULL;
        int src_remote;
        int dst_remote;

        src_remote = remote_split(src, &src_host, &src_path);
        dst_remote = remote_split(dst, &dst_host, &dst_path);
        if (src_remote && dst_remote) {
                die("both source and destination are remote");
        }
        if (era_init("") < 0) {
                return 2;
        }

        if (src_remote || dst_remote) {
                if (remote_copy(src_remote ? src_path : src,
                                dst_remote ? dst_path : dst,
                                src_host, dst_host) < 0) {
                        erasync_stop = 1;
                }
        } else if (sync_tree(src, dst) < 0) {
                erasync_stop = 1;
        }
        free(src_host);
        free(src_path);
        free(dst_host);
        free(dst_path);

        if (!erasync_stop) {
                era_finish();
        }
        summary();

        if (erasync_stop) {
                return 2;
        }
        return stats.errors ? 1 : 0;
}

int main(int argc, char **argv)
{
        int server;
        int operands;
        char *dst = NULL;

        server = scan_options(argc, argv);
        operands = argc - optind;
        if (!server && operands == 1) {
                dst = find_dest(argv[optind]);
        }
        if (!server && operands == 2) {
                dst = xstrdup(argv[optind + 1]);
        }
        if (dst) {
                read_config(argv[optind], dst);
        }
        server = parse_options(argc, argv, 0);
        operands = argc - optind;

        catch_signals();
        atexit(era_shutdown);

        if (server) {
                if (operands != 0) {
                        usage(2);
                }
                return run_server();
        }
        if (!dst) {
                usage(2);
        }
        show_config(dst);
        return run_sync(argv[optind], dst);
}
