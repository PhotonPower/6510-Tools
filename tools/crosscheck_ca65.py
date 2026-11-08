#!/usr/bin/env python3
"""Cross-checks the assembler (and with it the opcode table) against ca65/ld65 from cc65.

For every preferred opcode of the table (tools/dump_forms) one instruction with a typical
operand is assembled by m6510tool and by ca65 (--cpu 6502X); the bytes must be equal. For
absolute modes with a zero-page counterpart a second case forces the absolute form
(`LDA.W $0012` here, `lda a:$0012` in ca65). Instructions that ca65 does not know (some
undocumented names) are reported separately and do not fail the check.

    python3 tools/crosscheck_ca65.py build/m6510tool build/m6510_dump_forms

ca65 and ld65 are taken from PATH (or $CA65 / $LD65). Exit code 1 on any mismatch.
"""
import os
import re
import subprocess
import sys
import tempfile

# our name -> ca65 name (where they differ)
CA65_NAMES = {"SBX": "axs"}

ZP_OF = {"abs": "zp", "abs,X": "zp,X", "abs,Y": "zp,Y"}


def operand(mode):
    """Operand text in a syntax both assemblers accept."""
    return {
        "implied": "",
        "A": "a",
        "#imm": "#$12",
        "zp": "$12",
        "zp,X": "$12,x",
        "zp,Y": "$12,y",
        "abs": "$1234",
        "abs,X": "$1234,x",
        "abs,Y": "$1234,y",
        "(abs)": "($1234)",
        "(zp,X)": "($12,x)",
        "(zp),Y": "($12),y",
        "rel": "*+$10",
    }[mode]


def load_forms(dump_forms):
    out = subprocess.run([dump_forms], check=True, capture_output=True, text=True).stdout
    forms = []
    for line in out.splitlines():
        op, mn, mode, length, kind, pref = line.split()
        forms.append({"op": int(op, 16), "mn": mn, "mode": mode, "len": int(length), "kind": kind, "pref": pref == "1"})
    return forms


def build_cases(forms):
    have = {(f["mn"], f["mode"]) for f in forms}
    cases = []  # (description, ours, ca65, expected first byte, length)
    for f in forms:
        if not f["pref"]:
            continue
        mn, mode = f["mn"], f["mode"]
        ca = CA65_NAMES.get(mn, mn.lower())
        opnd = operand(mode)
        cases.append((f"{f['op']:02X} {mn} {mode}", f"{mn} {opnd}".strip(), f"{ca} {opnd}".strip(), f["op"], f["len"]))
        if mode in ZP_OF and (mn, ZP_OF[mode]) in have:
            suffix = mode[3:].lower()  # "", ",x", ",y"
            cases.append((f"{f['op']:02X} {mn} {mode} forced", f"{mn}.W $0012{suffix}", f"{ca} a:$0012{suffix}", f["op"], f["len"]))
    return cases


def run_ca65(cases, work, ca65, ld65):
    """Assembles the cases with ca65; returns (bytes, used cases, unsupported cases)."""
    active = list(cases)
    unsupported = []
    cfg = os.path.join(work, "x.cfg")
    with open(cfg, "w") as f:
        f.write("MEMORY { RAM: start = $1000, size = $E000, file = %O; }\nSEGMENTS { CODE: load = RAM, type = rw; }\n")
    for _ in range(20):
        src = os.path.join(work, "ca65.s")
        with open(src, "w") as f:
            f.write('.setcpu "6502X"\n.code\n')
            for c in active:
                f.write(f"        {c[2]}\n")
        r = subprocess.run([ca65, "-o", os.path.join(work, "x.o"), src], capture_output=True, text=True)
        if r.returncode == 0:
            break
        bad = set()
        for m in re.finditer(r"\((\d+)\):\s*Error|:(\d+):\s*Error", r.stderr + r.stdout):
            n = int(m.group(1) or m.group(2)) - 3  # two header lines, 1-based
            if 0 <= n < len(active):
                bad.add(n)
        if not bad:
            sys.exit("ca65 failed without line numbers:\n" + r.stderr + r.stdout)
        unsupported += [active[i] for i in sorted(bad)]
        active = [c for i, c in enumerate(active) if i not in bad]
    else:
        sys.exit("ca65 kept failing")
    out = os.path.join(work, "ca65.bin")
    r = subprocess.run([ld65, "-C", cfg, "-o", out, os.path.join(work, "x.o")], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ld65 failed:\n" + r.stderr + r.stdout)
    return open(out, "rb").read(), active, unsupported


def run_ours(cases, work, tool):
    src = os.path.join(work, "ours.asm")
    with open(src, "w") as f:
        f.write("        * = $1000\n")
        for c in cases:
            f.write(f"        {c[1]}\n")
    out = os.path.join(work, "ours.bin")
    r = subprocess.run([tool, "asm", src, "--raw", "--unstable", "-o", out], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("m6510tool asm failed:\n" + r.stderr + r.stdout)
    return open(out, "rb").read()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tool, dump_forms = sys.argv[1], sys.argv[2]
    ca65 = os.environ.get("CA65", "ca65")
    ld65 = os.environ.get("LD65", "ld65")
    cases = build_cases(load_forms(dump_forms))
    with tempfile.TemporaryDirectory() as work:
        theirs, active, unsupported = run_ca65(cases, work, ca65, ld65)
        ours = run_ours(active, work, tool)
    mismatches = []
    pos = 0
    for c in active:
        a, b = ours[pos:pos + c[4]], theirs[pos:pos + c[4]]
        if a != b or a[:1] != bytes([c[3]]):
            mismatches.append(f"{c[0]}: '{c[1]}' -> {a.hex(' ')}   ca65 '{c[2]}' -> {b.hex(' ')}")
        pos += c[4]
    if len(ours) != len(theirs):
        mismatches.append(f"total length differs: ours {len(ours)}, ca65 {len(theirs)}")
    print(f"assembler vs ca65: {len(active)} cases compared, {len(mismatches)} mismatches, "
          f"{len(unsupported)} not supported by ca65")
    if unsupported:
        print("  not supported by ca65: " + ", ".join(sorted({c[1].split()[0] for c in unsupported})))
    for m in mismatches[:50]:
        print("  MISMATCH " + m)
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
