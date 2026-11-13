#!/usr/bin/env python3
# Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause
"""host:path copy through a local stand-in for ssh. No dm-era."""

import os
import re
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "erasync"
RSH = Path(__file__).resolve().parent / "rsh-local"


def run(args, check=True, cwd=None):
    # A file written within a second of the era snapshot is looked at
    # again by the next run. Let the writes of the test settle first.
    time.sleep(1.2)
    cmd = [str(BIN), "-e", str(RSH), "--remote-path", str(BIN), *args]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"erasync {args} exited {proc.returncode}")
    return proc


def read(path):
    return Path(path).read_bytes()


def unwritten_bytes(path):
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


def check_config(root):
    """A sync block in the config file applies to that copy only.

    A relative local SRC matches the absolute path in the config, and
    without DEST the one from the sync line is used.
    """
    src = root / "cfg-src"
    dst = root / "cfg-dst"
    other = root / "cfg-other"
    src.mkdir()
    (src / "a-tester.qcow2").write_text("tester")
    (src / "keep").write_text("keep")
    config = root / "config"
    config.write_text(f"# test\nsync {src}/ local:{dst}\n"
                      "\t--exclude *-tester*.qcow2\n")

    out = run(["--config", str(config), "cfg-src", f"local:{dst}"],
              cwd=root)
    if "--exclude=*-tester*.qcow2" not in out.stdout:
        raise SystemExit("config options were not shown:\n" + out.stdout)
    if (dst / "a-tester.qcow2").exists() or read(dst / "keep") != b"keep":
        raise SystemExit("config exclude was not applied")

    (src / "new").write_text("new")
    out = run(["--config", str(config), str(src)])
    if f"DEST local:{dst} from" not in out.stdout:
        raise SystemExit("DEST from config was not shown:\n" + out.stdout)
    if read(dst / "new") != b"new" or (dst / "a-tester.qcow2").exists():
        raise SystemExit("copy to the DEST from config failed")
    run(["--config", str(config), str(src), f"local:{other}"])
    if read(other / "a-tester.qcow2") != b"tester":
        raise SystemExit("config exclude applied to another copy")

    config.write_text("--exclude='*.qcow2'\n")
    quoted = run(["--config", str(config), str(src), f"local:{dst}"],
                 check=False)
    if quoted.returncode != 2 or "quotes" not in quoted.stderr:
        raise SystemExit(f"quoted config: {quoted.returncode} {quoted.stderr}")


def main():
    if os.environ.get("ERASYNC_TEST_ON_ERA") != "1":
        print("skipped: run through tests/era.sh, which puts TMPDIR on dm-era")
        sys.exit(77)
    if not BIN.is_file():
        raise SystemExit(f"missing {BIN}")
    if not os.access(RSH, os.X_OK):
        raise SystemExit(f"{RSH} is not executable")

    help_run = run(["--help"])
    if "HOST:PATH" not in help_run.stdout or "--remote-path" not in help_run.stdout:
        raise SystemExit("help text missing host:path")

    server = subprocess.run([str(BIN), "--server"], stdin=subprocess.DEVNULL,
                            capture_output=True, text=True)
    if server.returncode != 2:
        raise SystemExit(f"--server with no peer exited {server.returncode}")

    both = run(["local:/tmp/a", "local:/tmp/b"], check=False)
    if both.returncode != 2 or "remote" not in both.stderr:
        raise SystemExit(f"both remote: {both.returncode} {both.stderr}")

    with tempfile.TemporaryDirectory(prefix="erasync-remote-") as td:
        root = Path(td)
        src = root / "src"
        dst = root / "dst"
        src.mkdir()
        (src / "sub").mkdir(mode=0o750)
        payload = b"a\0b\xff" + b"Z" * 10000
        (src / "sub" / "c.txt").write_bytes(payload)
        os.chmod(src / "sub" / "c.txt", 0o640)
        stamp = 1_600_000_000_123456789
        os.utime(src / "sub" / "c.txt", ns=(stamp, stamp))
        (src / "a file").write_text("spaces\n")
        (src / "empty").write_bytes(b"")
        (src / "link").symlink_to("target-name")
        os.mkfifo(src / "fifo", 0o640)
        sparse = src / "sparse"
        with sparse.open("wb") as fh:
            fh.write(b"AAAA")
            fh.truncate(100000)
            fh.seek(50000)
            fh.write(b"BBBB")
        os.chmod(src / "sub", 0o750)
        dir_stamp = 1_650_000_000_000000000
        os.utime(src / "sub", ns=(dir_stamp, dir_stamp))

        colon_name = src / "foo:bar"
        colon_name.write_text("colon\n")

        first = run(["-v", str(src), f"local:{dst}"])
        if f"create {dst}" not in first.stdout:
            raise SystemExit(first.stdout)
        if "create sub/c.txt" not in first.stdout:
            raise SystemExit(first.stdout)
        if "full copy" not in first.stdout:
            raise SystemExit(first.stdout)
        if not re.search(r"\d+ / \d+ / \d+", first.stderr):
            raise SystemExit("missing transfer progress:\n" + first.stderr)
        if read(dst / "sub" / "c.txt") != payload:
            raise SystemExit("push content mismatch")
        if read(dst / "a file") != b"spaces\n":
            raise SystemExit("spaced name mismatch")
        if read(dst / "empty") != b"":
            raise SystemExit("empty file mismatch")
        if read(dst / "sparse") != read(sparse):
            raise SystemExit("sparse content mismatch")
        if os.readlink(dst / "link") != "target-name":
            raise SystemExit("symlink target mismatch")
        if not stat.S_ISFIFO(os.lstat(dst / "fifo").st_mode):
            raise SystemExit("fifo was not replicated")
        st = os.lstat(dst / "sub" / "c.txt")
        if stat.S_IMODE(st.st_mode) != 0o640 or st.st_mtime_ns != stamp:
            raise SystemExit(f"file metadata {oct(st.st_mode)} {st.st_mtime_ns}")
        if stat.S_IMODE(os.lstat(dst / "sub").st_mode) != 0o750:
            raise SystemExit("directory mode mismatch")
        if os.lstat(dst / "sub").st_mtime_ns != dir_stamp:
            raise SystemExit("directory mtime mismatch")
        if read(dst / "foo:bar") != b"colon\n":
            raise SystemExit("colon in a file name was parsed as a host")

        sibling = os.lstat(dst / "empty")
        (src / "a file").write_text("changed\n")
        second = run([str(src), f"local:{dst}"])
        if read(dst / "a file") != b"changed\n" or "update a file" not in second.stdout:
            raise SystemExit(second.stdout)
        if "\ncreate " in "\n" + second.stdout:
            raise SystemExit("unchanged path was created again:\n" + second.stdout)
        again = os.lstat(dst / "empty")
        if (again.st_ino, again.st_mtime_ns, again.st_ctime_ns) != (
                sibling.st_ino, sibling.st_mtime_ns, sibling.st_ctime_ns):
            raise SystemExit("unchanged file was touched")

        third = run(["-v", str(src), f"local:{dst}"])
        if "skip a file" not in third.stdout or "\nupdate " in "\n" + third.stdout:
            raise SystemExit("third sync was not a no-op:\n" + third.stdout)

        pull_dst = root / "pull"
        pulled = run(["-v", f"local:{src}", str(pull_dst)])
        if f"create {pull_dst}" not in pulled.stdout:
            raise SystemExit(pulled.stdout)
        if read(pull_dst / "sub" / "c.txt") != payload:
            raise SystemExit("pull content mismatch")
        if os.readlink(pull_dst / "link") != "target-name":
            raise SystemExit("pull symlink mismatch")
        if read(pull_dst / "sparse") != read(sparse):
            raise SystemExit("pull sparse mismatch")

        one = root / "one.img"
        one.write_bytes(b"a" * 1000 + b"\0b")
        one_dst = root / "one-copy.img"
        copied = run([str(one), f"local:{one_dst}"])
        if read(one_dst) != read(one) or "create one-copy.img" not in copied.stdout:
            raise SystemExit(copied.stdout + copied.stderr)
        blank = root / "unwritten.img"
        blank_dst = root / "unwritten-copy.img"
        bfd = os.open(blank, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
        os.write(bfd, b"HEAD")
        os.posix_fallocate(bfd, 4096, 8 * 1024 * 1024)
        os.lseek(bfd, 4096 + 8 * 1024 * 1024, os.SEEK_SET)
        os.write(bfd, b"TAIL")
        os.close(bfd)
        zeros = unwritten_bytes(blank)
        pushed = run([str(blank), f"local:{blank_dst}"])
        if read(blank_dst) != read(blank):
            raise SystemExit("unwritten extent content mismatch")
        match = re.search(r"copied (\d+) bytes", pushed.stdout)
        if zeros > 1024 * 1024 and (not match or int(match.group(1)) >= zeros):
            raise SystemExit(
                f"unwritten extents were read ({match.group(1) if match else '?'} "
                f"of {zeros}):\n" + pushed.stdout
            )
        into = root / "into-dir"
        into.mkdir()
        (into / "keep").write_text("stay")
        run([f"local:{one}", str(into)])
        if read(into / "one.img") != read(one) or read(into / "keep") != b"stay":
            raise SystemExit("pull into a directory removed a sibling")

        (dst / "extra.txt").write_text("gone")
        (dst / "extradir").mkdir()
        (dst / "extradir" / "f").write_text("gone")
        outside = root / "outside"
        outside.mkdir()
        secret = outside / "secret"
        secret.write_text("keep")
        os.symlink(outside, dst / "linkdir")
        deleted = run(["--delete", str(src), f"local:{dst}"])
        if (dst / "extra.txt").exists() or (dst / "extradir").exists() or (dst / "linkdir").exists():
            raise SystemExit("delete left extras behind")
        if secret.read_text() != "keep":
            raise SystemExit("delete followed a symlink")
        if "delete extra.txt" not in deleted.stdout:
            raise SystemExit(deleted.stdout)

        # A pull deletes on the local side, which must print and count it.
        (pull_dst / "pull-extra.txt").write_text("gone")
        pull_deleted = run(["--delete", f"local:{src}", str(pull_dst)])
        if (pull_dst / "pull-extra.txt").exists():
            raise SystemExit("pull delete left an extra behind")
        if "delete pull-extra.txt" not in pull_deleted.stdout:
            raise SystemExit("pull delete was not reported:\n" + pull_deleted.stdout)
        if "deleted 1," not in pull_deleted.stdout:
            raise SystemExit("pull delete was not counted:\n" + pull_deleted.stdout)

        held = os.lstat(dst / "empty")
        (src / "a file").write_text("dry\n")
        dry = run(["-n", str(src), f"local:{dst}"])
        if read(dst / "a file") != b"changed\n":
            raise SystemExit("dry-run wrote a file")
        if os.lstat(dst / "empty").st_ctime_ns != held.st_ctime_ns:
            raise SystemExit("dry-run touched an unchanged file")
        if "update a file" not in dry.stdout or "dry-run:" not in dry.stdout:
            raise SystemExit(dry.stdout + dry.stderr)
        if not re.search(r"update a file\n +up to \S+ \(estimate\)\n", dry.stdout):
            raise SystemExit("dry-run estimate missing:\n" + dry.stdout)
        if not re.search(r"up to [1-9]\S* to copy \(estimate\)$", dry.stdout):
            raise SystemExit("dry-run summary estimate missing:\n" + dry.stdout)
        # A pull estimates on the server: the lines come through, the sum
        # in the summary does not.
        dry_pull = run(["-n", f"local:{src}", str(pull_dst)])
        if read(pull_dst / "a file") == b"dry\n":
            raise SystemExit("dry-run pull wrote a file")
        if not re.search(r"update a file\n +up to \S+ \(estimate\)\n", dry_pull.stdout):
            raise SystemExit("dry-run pull estimate missing:\n" + dry_pull.stdout)
        if "copied 0 bytes" not in dry_pull.stdout:
            raise SystemExit("dry-run pull summary:\n" + dry_pull.stdout)
        if (root / "dry-missing").exists():
            raise SystemExit("dry-run created a directory")
        dry_new = run(["-n", str(src), f"local:{root / 'dry-missing'}"])
        if (root / "dry-missing").exists():
            raise SystemExit("dry-run created the destination")
        if "create " not in dry_new.stdout:
            raise SystemExit(dry_new.stdout)
        if not re.search(r"\n +\S+, new file \(estimate\)\n", dry_new.stdout):
            raise SystemExit("dry-run create estimate missing:\n" + dry_new.stdout)

        slash = run([str(one), f"local:{root / 'missing-dir'}/"], check=False)
        if slash.returncode != 2 or "not a directory" not in slash.stderr:
            raise SystemExit(f"trailing slash: {slash.returncode} {slash.stderr}")

        missing = run([str(one), f"local:{root / 'no-such' / 'f.img'}"], check=False)
        if missing.returncode != 2 or "No such file" not in missing.stderr:
            raise SystemExit(f"missing parent exit {missing.returncode} {missing.stderr}")

        local_colon = root / "colon-out"
        run([str(src / "foo:bar"), str(local_colon)])
        if read(local_colon) != b"colon\n":
            raise SystemExit("local name with a colon was sent to ssh")

        push = root / "push"
        push.mkdir()
        check_exclude(push, lambda args, s, d: run([*args, str(s), f"local:{d}"]))
        pull = root / "pull-ex"
        pull.mkdir()
        check_exclude(pull, lambda args, s, d: run([*args, f"local:{s}", str(d)]))
        check_config(root)

    print("ok")


if __name__ == "__main__":
    main()
