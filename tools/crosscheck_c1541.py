#!/usr/bin/env python3
"""Cross-check of the D64 reader against c1541 (VICE).

c1541 formats a disk and writes files of many sizes (empty, exactly one block, block boundaries, long files, enough files
for a directory over several sectors); then `m6510tool d64 ls/extract/info` must list and extract the same as
`c1541 -list/-read`.

Usage: crosscheck_c1541.py <m6510tool> [c1541]
"""
import os
import random
import re
import subprocess
import sys
import tempfile


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


def listing(text):
    """(blocks, name, type) of the files and the free blocks from a 1541-style directory listing."""
    files, free = [], None
    for line in text.splitlines():
        m = re.match(r'^\s*(\d+)\s+"([^"]*)"\s+(\*?)(\w{3})(<?)', line)
        if m and not line.lstrip().startswith("0 \""):
            files.append((int(m.group(1)), m.group(2).strip().lower(), m.group(4).lower()))
        m = re.match(r"^\s*(\d+) blocks free", line)
        if m:
            free = int(m.group(1))
    return files, free


def main():
    tool = sys.argv[1]
    c1541 = sys.argv[2] if len(sys.argv) > 2 else "c1541"
    rnd = random.Random(1541)
    sizes = [2, 3, 254, 255, 256, 508, 509, 1000, 4096, 20000] + [rnd.randint(2, 3000) for _ in range(14)]
    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "check.d64")
        run([c1541, "-format", "crosscheck,cc", "d64", disk])
        names = []
        for i, size in enumerate(sizes):
            host = os.path.join(tmp, f"f{i}.prg")
            with open(host, "wb") as f:
                f.write(bytes(rnd.randrange(256) for _ in range(size)))
            name = f"file{i}"
            run([c1541, disk, "-write", host, name])
            names.append((name, host))
        theirs, their_free = listing(run([c1541, disk, "-list"]))
        ours, our_free = listing(run([tool, "d64", "ls", disk]))
        problems = []
        if sorted(theirs) != sorted(ours):
            problems.append(f"directory differs:\n  c1541: {sorted(theirs)}\n  ours:  {sorted(ours)}")
        if their_free != our_free:
            problems.append(f"free blocks: c1541 {their_free}, ours {our_free}")
        for name, host in names:
            a, b = os.path.join(tmp, "a.bin"), os.path.join(tmp, "b.bin")
            run([c1541, disk, "-read", name, a])
            run([tool, "d64", "extract", disk, name, "-o", b])
            with open(a, "rb") as fa, open(b, "rb") as fb, open(host, "rb") as fh:
                da, db, dh = fa.read(), fb.read(), fh.read()
            if da != db or db != dh:
                problems.append(f"{name}: c1541 {len(da)} bytes, ours {len(db)} bytes, written {len(dh)} bytes")
        print(f"c1541 cross-check: {len(names)} files, {len(problems)} differences")
        for p in problems:
            print(p)
        return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
