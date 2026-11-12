#!/usr/bin/env python3
# Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause
"""Tree sync without dm-era: files, directories, symlinks, and special files."""

import os
import re
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BIN = Path(__file__).resolve().parents[1] / "erasync"


def run(args, check=True):
    # A file written within a second of the era snapshot is looked at
    # again by the next run. Let the writes of the test settle first.
    time.sleep(1.2)
    proc = subprocess.run([str(BIN), *args], capture_output=True, text=True)
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"erasync {args} exited {proc.returncode}")
    return proc


def read(path):
    return Path(path).read_bytes()


def ns(path):
    st = os.lstat(path)
    return st.st_mtime_ns


def unwritten_bytes(path):
    """Bytes filefrag reports as unwritten. 0 when the filesystem has none."""
    try:
        proc = subprocess.run(["filefrag", "-v", str(path)], capture_output=True, text=True)
    except FileNotFoundError:
        return 0
    if proc.returncode != 0:
        return 0
    bsize = 4096
    for line in proc.stdout.splitlines():
        if "blocks of" in line:
            bsize = int(line.split("blocks of", 1)[1].split()[0])
            break
    total = 0
    for line in proc.stdout.splitlines():
        if "unwritten" not in line:
            continue
        fields = [part.strip() for part in line.split(":")]
        if not fields[0].isdigit():
            continue
        try:
            total += int(fields[3]) * bsize
        except (IndexError, ValueError):
            continue
    return total


def copied_bytes(text):
    match = re.search(r"copied (\d+) bytes", text)
    if not match:
        raise SystemExit("no copied count:\n" + text)
    return int(match.group(1))


def check_exclude(root, sync):
    """--exclude skips entries, and --delete keeps excluded ones in DEST.

    sync(args, src, dst) runs one copy, local or remote.
    """
    src = root / "ex-src"
    dst = root / "ex-dst"
    (src / "vm").mkdir(parents=True)
    (src / "vm" / "ad-f44-tester.qcow2").write_text("tester")
    (src / "vm" / "ad-f44-base.qcow2").write_text("base")
    (src / "logs").mkdir()
    (src / "logs" / "run.log").write_text("log")
    (src / "deep" / "logs").mkdir(parents=True)
    (src / "deep" / "logs" / "keep.log").write_text("deep")
    (src / "cache").mkdir()
    (src / "cache" / "blob").write_text("cache")
    excludes = ["--exclude=*-tester*.qcow2", "--exclude", "/logs",
                "--exclude=cache/"]

    sync(excludes, src, dst)
    if (dst / "vm" / "ad-f44-tester.qcow2").exists():
        raise SystemExit("excluded file was copied")
    if read(dst / "vm" / "ad-f44-base.qcow2") != b"base":
        raise SystemExit("file next to an excluded one was not copied")
    if (dst / "logs").exists() or (dst / "cache").exists():
        raise SystemExit("excluded directory was copied")
    if read(dst / "deep" / "logs" / "keep.log") != b"deep":
        raise SystemExit("an anchored pattern matched below the top")

    (dst / "vm" / "ad-f44-tester.qcow2").write_text("dest only")
    (dst / "logs").mkdir()
    (dst / "logs" / "old.log").write_text("dest only")
    (dst / "vm" / "gone.qcow2").write_text("gone")
    sync(["--delete", *excludes], src, dst)
    if read(dst / "vm" / "ad-f44-tester.qcow2") != b"dest only":
        raise SystemExit("--delete removed an excluded file")
    if read(dst / "logs" / "old.log") != b"dest only":
        raise SystemExit("--delete removed an excluded directory")
    if (dst / "vm" / "gone.qcow2").exists():
        raise SystemExit("--delete kept a file that is not excluded")


def main():
    if os.environ.get("ERASYNC_TEST_ON_ERA") != "1":
        print("skipped: run through tests/era.sh, which puts TMPDIR on dm-era")
        sys.exit(77)
    if not BIN.is_file():
        raise SystemExit(f"missing {BIN}")

    help_run = run(["--help"])
    if "SRC DEST" not in help_run.stdout:
        raise SystemExit("help text missing usage")
    bad = run(["--nope", "a", "b"], check=False)
    if bad.returncode != 2:
        raise SystemExit(f"bad option exit {bad.returncode}")

    with tempfile.TemporaryDirectory(prefix="erasync-") as td:
        root = Path(td)
        src = root / "src"
        dst = root / "dst"
        src.mkdir()
        (src / "sub").mkdir(mode=0o750)
        (src / "sub" / "c.txt").write_bytes(b"a\0b\xff")
        (src / "a file").write_text("spaces\n")
        (src / "räksmörgås").write_text("unicode\n")
        (src / "empty").write_bytes(b"")
        (src / "link").symlink_to("target-name")
        os.mkfifo(src / "fifo", 0o640)
        try:
            os.mknod(src / "sock", stat.S_IFSOCK | 0o600)
            have_sock = True
        except OSError:
            have_sock = False
        sparse = src / "sparse"
        with sparse.open("wb") as fh:
            fh.write(b"AAAA")
            fh.truncate(100000)
            fh.seek(50000)
            fh.write(b"BBBB")
        file_mode = src / "sub" / "c.txt"
        os.chmod(file_mode, 0o640)
        os.chmod(src / "sub", 0o750)
        stamp = 1_600_000_000_123456789
        os.utime(file_mode, ns=(stamp, stamp))

        first = run([str(src), str(dst)])
        if "create sub/c.txt" not in first.stdout:
            raise SystemExit(first.stdout)
        if read(dst / "sub" / "c.txt") != b"a\0b\xff":
            raise SystemExit("binary content mismatch")
        if read(dst / "a file") != b"spaces\n":
            raise SystemExit("spaced name mismatch")
        if read(dst / "räksmörgås") != b"unicode\n":
            raise SystemExit("unicode name mismatch")
        if read(dst / "empty") != b"":
            raise SystemExit("empty file mismatch")
        if read(dst / "sparse") != read(sparse):
            raise SystemExit("sparse content mismatch")
        if os.readlink(dst / "link") != "target-name":
            raise SystemExit("symlink target mismatch")
        if not stat.S_ISFIFO(os.lstat(dst / "fifo").st_mode):
            raise SystemExit("fifo was not replicated")
        if have_sock and not stat.S_ISSOCK(os.lstat(dst / "sock").st_mode):
            raise SystemExit("socket was not replicated")
        st = os.lstat(dst / "sub" / "c.txt")
        if stat.S_IMODE(st.st_mode) != 0o640:
            raise SystemExit(f"mode {oct(st.st_mode)}")
        if st.st_mtime_ns != stamp:
            raise SystemExit(f"mtime {st.st_mtime_ns}")
        if stat.S_IMODE(os.lstat(dst / "sub").st_mode) != 0o750:
            raise SystemExit("directory mode mismatch")

        sibling = os.lstat(dst / "empty")
        (src / "a file").write_text("changed\n")
        second = run([str(src), str(dst)])
        if read(dst / "a file") != b"changed\n":
            raise SystemExit("update did not land")
        if "update a file" not in second.stdout:
            raise SystemExit(second.stdout)
        if "\ncreate " in "\n" + second.stdout or "\nupdate sub" in second.stdout:
            raise SystemExit("unchanged path was rewritten:\n" + second.stdout)
        again = os.lstat(dst / "empty")
        if (again.st_ino, again.st_mtime_ns, again.st_ctime_ns) != (
            sibling.st_ino, sibling.st_mtime_ns, sibling.st_ctime_ns):
            raise SystemExit("unchanged file was touched")

        third = run(["-v", str(src), str(dst)])
        if "skip a file" not in third.stdout or "update " in third.stdout:
            raise SystemExit("third sync was not a no-op:\n" + third.stdout)

        outside = root / "outside"
        outside.mkdir()
        secret = outside / "secret"
        secret.write_text("keep")
        os.symlink(outside, dst / "linkdir")
        (dst / "extra.txt").write_text("gone")
        (dst / "extradir").mkdir()
        (dst / "extradir" / "f").write_text("gone")
        (src / "sub").mkdir(exist_ok=True)
        (src / "sub" / "in").write_text("inside")
        # linkdir is not in the source. sub/ already exists; add a new file.
        deleted = run(["--delete", str(src), str(dst)])
        if (dst / "extra.txt").exists() or (dst / "extradir").exists():
            raise SystemExit("delete left extras behind")
        if (dst / "linkdir").exists():
            raise SystemExit("delete followed or kept a symlink")
        if secret.read_text() != "keep":
            raise SystemExit("delete followed a symlink")
        if "delete extra.txt" not in deleted.stdout:
            raise SystemExit(deleted.stdout)

        os.symlink(outside, dst / "sub2")
        (src / "sub2").mkdir()
        (src / "sub2" / "in").write_text("real")
        run([str(src), str(dst)])
        if (dst / "sub2").is_symlink() or read(dst / "sub2" / "in") != b"real":
            raise SystemExit("symlink was followed instead of replaced")
        if secret.read_text() != "keep":
            raise SystemExit("replaced symlink was followed")

        was = src / "was"
        was.write_text("file")
        run([str(src), str(dst)])
        was.unlink()
        was.symlink_to("elsewhere")
        run([str(src), str(dst)])
        if not (dst / "was").is_symlink() or os.readlink(dst / "was") != "elsewhere":
            raise SystemExit("file was not replaced by a symlink")

        held = os.lstat(dst / "empty")
        (src / "a file").write_text("dry\n")
        dry = run(["-n", str(src), str(dst)])
        if read(dst / "a file") != b"changed\n":
            raise SystemExit("dry-run wrote a file")
        if os.lstat(dst / "empty").st_ctime_ns != held.st_ctime_ns:
            raise SystemExit("dry-run touched an unchanged file")
        if "update a file" not in dry.stdout:
            raise SystemExit(dry.stdout)
        if "dry-run:" not in dry.stdout:
            raise SystemExit("summary missing dry-run marker")

        overlap = run([str(src), str(src)], check=False)
        if overlap.returncode != 2 or "overlap" not in overlap.stderr:
            raise SystemExit(f"same-dir overlap: {overlap.returncode} {overlap.stderr}")
        nested = src / "nested-dest"
        nested.mkdir()
        inside = run([str(src), str(nested)], check=False)
        if inside.returncode != 2 or "overlap" not in inside.stderr:
            raise SystemExit(f"dest-inside-src: {inside.returncode} {inside.stderr}")
        if any(nested.iterdir()):
            raise SystemExit("overlap check wrote into the destination")
        reverse = run([str(nested), str(src)], check=False)
        if reverse.returncode != 2 or "overlap" not in reverse.stderr:
            raise SystemExit(f"src-inside-dest: {reverse.returncode} {reverse.stderr}")

        one_src = root / "one.img"
        one_src.write_bytes(b"a" * 1000 + b"\0b")
        os.chmod(one_src, 0o640)
        one_stamp = 1_700_000_000_111111111
        os.utime(one_src, ns=(one_stamp, one_stamp))
        one_dst = root / "one-copy.img"
        copied = run([str(one_src), str(one_dst)])
        if "create one-copy.img" not in copied.stdout:
            raise SystemExit(copied.stdout)
        if read(one_dst) != read(one_src):
            raise SystemExit("single file content")
        one_st = os.lstat(one_dst)
        if stat.S_IMODE(one_st.st_mode) != 0o640 or one_st.st_mtime_ns != one_stamp:
            raise SystemExit("single file metadata")
        skipped = run(["-v", str(one_src), str(one_dst)])
        if "skip one-copy.img" not in skipped.stdout:
            raise SystemExit(skipped.stdout)
        if os.lstat(one_dst).st_ino != one_st.st_ino:
            raise SystemExit("unchanged single file was rewritten")
        one_src.write_bytes(b"zz")
        os.utime(one_src, ns=(one_stamp + 1_000_000_000,) * 2)
        updated = run([str(one_src), str(one_dst)])
        if read(one_dst) != b"zz" or "update one-copy.img" not in updated.stdout:
            raise SystemExit(updated.stdout)
        into = root / "into-dir"
        into.mkdir()
        (into / "keep").write_text("stay")
        run(["--delete", str(one_src), str(into)])
        if read(into / "one.img") != b"zz" or read(into / "keep") != b"stay":
            raise SystemExit("file copied into a directory removed a sibling")
        same = run([str(one_src), str(one_src)], check=False)
        if same.returncode != 2 or "overlap" not in same.stderr:
            raise SystemExit(f"same file: {same.returncode} {same.stderr}")
        empty = root / "unwritten.img"
        empty_dst = root / "unwritten-copy.img"
        efd = os.open(empty, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
        os.write(efd, b"HEAD")
        os.posix_fallocate(efd, 4096, 8 * 1024 * 1024)
        os.lseek(efd, 4096 + 8 * 1024 * 1024, os.SEEK_SET)
        os.write(efd, b"TAIL")
        os.close(efd)
        zeros = unwritten_bytes(empty)
        emptied = run([str(empty), str(empty_dst)])
        if read(empty_dst) != read(empty):
            raise SystemExit("unwritten extent content mismatch")
        if zeros > 1024 * 1024 and copied_bytes(emptied.stdout) >= zeros:
            raise SystemExit(
                f"unwritten extents were read ({copied_bytes(emptied.stdout)} "
                f"of {zeros}):\n" + emptied.stdout
            )
        one_src.write_bytes(b"dry-file")
        dry_file = run(["-n", str(one_src), str(one_dst)])
        if read(one_dst) != b"zz" or "update one-copy.img" not in dry_file.stdout:
            raise SystemExit("dry-run of a file wrote or stayed quiet:\n" + dry_file.stdout)
        missing = run([str(one_src), str(root / "no-such" / "f.img")], check=False)
        if missing.returncode != 2:
            raise SystemExit(f"missing parent exit {missing.returncode}")

        check_exclude(root, lambda args, s, d: run([*args, str(s), str(d)]))

    print("ok")


if __name__ == "__main__":
    main()
