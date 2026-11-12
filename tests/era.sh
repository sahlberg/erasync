#!/bin/bash
# Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause
#
# Incremental copies on real dm-era devices, built on loop files. Needs
# root, dmsetup, losetup, mkfs.ext4 and era_invalidate.
#
#   tests/era.sh [tree|local|push|pull]...     (default: all of them)
#
# tree runs basic.py and remote.py with TMPDIR on a dm-era filesystem, and
# checks that paths which are not on dm-era are refused. local, push and
# pull check incremental copies: the content and the bytes copied after
# every step. push and pull go through tests/rsh-local instead of ssh.
# Each mode gets fresh devices and a fresh era log.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/../erasync
RSH=$HERE/rsh-local
MODES=${*:-tree local push pull}
FAILED=0

if [ "$(id -u)" != 0 ]; then
        echo "era.sh: needs root, skipped"
        exit 77
fi
for tool in dmsetup losetup mkfs.ext4 era_invalidate; do
        if ! command -v $tool > /dev/null; then
                echo "era.sh: $tool not found, skipped"
                exit 77
        fi
done

W=$(mktemp -d /tmp/erasync-era.XXXXXX)
NAME=erasync-test-$$
LOOPS=

cleanup() {
        umount "$W/src" "$W/dst" 2> /dev/null
        dmsetup remove "$NAME-src" 2> /dev/null
        dmsetup remove "$NAME-dst" 2> /dev/null
        for l in $LOOPS; do
                losetup -d "$l" 2> /dev/null
        done
        LOOPS=
        rm -rf "$W/src" "$W/dst" "$W"/*.data "$W"/*.meta
}

finish() {
        cleanup
        rm -rf "$W"
}
trap finish EXIT

# make_era side [era block size in sectors, default 128: 64 KiB]
make_era() {
        local side=$1 block=${2:-128} data meta

        truncate -s 512M "$W/$side.data"
        truncate -s 32M "$W/$side.meta"
        data=$(losetup -f --show "$W/$side.data")
        meta=$(losetup -f --show "$W/$side.meta")
        LOOPS="$LOOPS $data $meta"
        dmsetup create "$NAME-$side" \
                --table "0 $((512 * 2048)) era $meta $data $block"
        mkfs.ext4 -q "/dev/mapper/$NAME-$side"
        mkdir -p "$W/$side"
        mount "/dev/mapper/$NAME-$side" "$W/$side"
        rm -rf "$W/$side/lost+found"
}

fail() {
        echo "FAIL ($MODE, $STEP): $*"
        sed 's/^/    /' "$W/out"
        FAILED=1
}

# Run one sync and check that the trees match.
run_sync() {
        sleep 2
        case $MODE in
        local)
                "$BIN" -v "$@" "$W/src/" "$W/dst" ;;
        push)
                "$BIN" -e "$RSH" --remote-path "$BIN" -v "$@" \
                        "$W/src/" "local:$W/dst" ;;
        pull)
                "$BIN" -e "$RSH" --remote-path "$BIN" -v "$@" \
                        "local:$W/src/" "$W/dst" ;;
        esac > "$W/out" 2>&1
        RC=$?
        COPIED=$(sed -n 's/.*copied \([0-9]*\) bytes.*/\1/p' "$W/out")
        if [ $RC != 0 ]; then
                fail "exit $RC"
        elif ! diff -r --no-dereference "$W/src" "$W/dst" > /dev/null; then
                fail "trees differ"
        fi
}

expect_copied() {
        if [ -z "$COPIED" ] || [ "$COPIED" -lt "$1" ] ||
           [ "$COPIED" -gt "$2" ]; then
                fail "copied ${COPIED:-?} bytes, expected $1 to $2"
        fi
}

expect_output() {
        if ! grep -q -- "$1" "$W/out"; then
                fail "no \"$1\" in the output"
        fi
}

write_at() {
        dd if=/dev/urandom of="$1" bs=4K count=1 seek=$(($2 / 4096)) \
                conv=notrunc status=none
}

M=$((1024 * 1024))

# The tree tests, and paths that are not on dm-era.
run_tree() {
        local t

        # 4 KiB era blocks: one ext4 block, so two small files never share
        # an era block and a write to one does not mark the other dirty.
        make_era src 8
        mkdir "$W/src/tmp"
        for t in basic remote; do
                STEP=$t.py
                ERASYNC_TEST_ON_ERA=1 TMPDIR="$W/src/tmp" \
                        python3 "$HERE/$t.py" > "$W/out" 2>&1 ||
                        fail "failed"
        done

        # $W is in /tmp, which is not on dm-era.
        mkdir -p "$W/plain"
        echo x > "$W/plain/f"
        STEP="source not on dm-era"
        "$BIN" "$W/plain" "$W/src/tmp/out" > "$W/out" 2>&1
        RC=$?
        if [ $RC != 2 ] || ! grep -q "not on a dm-era device" "$W/out"; then
                fail "exit $RC"
        fi
        STEP="destination not on dm-era"
        "$BIN" "$W/src/tmp" "$W/plain/out" > "$W/out" 2>&1
        RC=$?
        if [ $RC != 2 ] || ! grep -q "not on a dm-era device" "$W/out"; then
                fail "exit $RC"
        fi
        rm -rf "$W/plain"
}

for MODE in $MODES; do
        cleanup
        rm -rf /var/lib/erasync/era
        if [ "$MODE" = tree ]; then
                run_tree
                if [ $FAILED = 0 ]; then
                        echo "era.sh: tree ok"
                fi
                continue
        fi
        make_era src
        make_era dst

        dd if=/dev/urandom of="$W/src/big.img" bs=1M count=64 status=none
        dd if=/dev/urandom of="$W/src/sparse.img" bs=1M count=4 seek=32 \
                status=none
        echo hello > "$W/src/small.txt"
        mkdir "$W/src/dir"
        echo inner > "$W/src/dir/inner.txt"
        ln -s small.txt "$W/src/link"
        sync

        STEP="first copy"
        run_sync
        expect_copied $((68 * M)) $((69 * M))
        if command -v getfattr > /dev/null &&
           ! getfattr --only-values -n user.erasync.era "$W/dst/big.img" \
                        2> /dev/null | grep -q '^v1 '; then
                fail "big.img has no v1 era record"
        fi

        STEP="no change"
        run_sync
        expect_copied 0 0

        STEP="source write"
        write_at "$W/src/big.img" $((10 * M))
        run_sync
        expect_copied 1 $((128 * 1024))

        STEP="destination write"
        write_at "$W/dst/big.img" $((20 * M))
        run_sync
        expect_copied 1 $((128 * 1024))
        expect_output "destination was written after its recorded era"

        STEP="writes on both sides"
        write_at "$W/src/big.img" $((30 * M))
        write_at "$W/dst/big.img" $((40 * M))
        run_sync
        expect_copied 1 $((256 * 1024))

        STEP="append and punch"
        dd if=/dev/urandom of="$W/src/big.img" bs=1M count=1 seek=64 \
                conv=notrunc status=none
        fallocate -p -o $((33 * M)) -l $M "$W/src/sparse.img"
        run_sync
        expect_copied $M $((M + 256 * 1024))

        STEP="metadata only"
        chmod 600 "$W/src/small.txt"
        touch -d 2020-01-01 "$W/src/small.txt"
        run_sync
        expect_output "metadata 1,"
        expect_copied 0 0

        STEP="new file and delete"
        dd if=/dev/urandom of="$W/src/new.img" bs=1M count=2 status=none
        rm "$W/src/dir/inner.txt"
        run_sync --delete
        expect_output "deleted 1,"
        expect_copied $((2 * M)) $((2 * M + 256 * 1024))

        STEP="destination write to a new file"
        write_at "$W/dst/new.img" 0
        run_sync
        expect_copied 1 $((256 * 1024))

        # big.img is 65 MiB of data, sparse.img 3 MiB, new.img 2 MiB.
        STEP="force one file"
        run_sync --force='big*.img'
        expect_copied $((65 * M)) $((65 * M + 256 * 1024))
        expect_output "update big.img (full copy: forced"

        STEP="force everything"
        run_sync --force
        expect_copied $((70 * M)) $((70 * M + 256 * 1024))
        expect_output "update new.img (full copy: forced"

        STEP="after forcing"
        run_sync
        expect_copied 0 0

        if [ $FAILED = 0 ]; then
                echo "era.sh: $MODE ok"
        fi
done

exit $FAILED
