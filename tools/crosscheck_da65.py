#!/usr/bin/env python3
"""Cross-checks the decoder (the opcode table) against da65 from cc65.

Every one of the 256 opcodes is written into its own small file (opcode + operand bytes $21 $43,
so that branch and address targets lie outside the file and da65 prints no labels for them) and
disassembled by da65 --cpu 6502x. Mnemonic, addressing mode (from the operand syntax) and length
must match the decoder. Opcodes that da65 shows as `.byte` are counted separately; for a
documented opcode that is a mismatch.

    python3 tools/crosscheck_da65.py build/m6510_dump_forms

da65 is taken from PATH (or $DA65). Exit code 1 on any mismatch.
"""
import os
import re
import subprocess
import sys
import tempfile

# our name -> da65 name (where they differ)
DA65_NAMES = {"SBX": "axs"}


def load_forms(dump_forms):
    out = subprocess.run([dump_forms], check=True, capture_output=True, text=True).stdout
    forms = []
    for line in out.splitlines():
        op, mn, mode, length, kind, pref = line.split()
        forms.append({"op": int(op, 16), "mn": mn, "mode": mode, "len": int(length), "kind": kind})
    return forms


def mode_of(mnemonic, opnd, length):
    """Addressing mode from da65's operand text (labels are allowed, the length decides zp/abs)."""
    o = opnd.replace(" ", "").lower()
    if o.startswith("a:"):
        o = o[2:]
    if o == "":
        return "implied"
    if o == "a":
        return "A"
    if o.startswith("#"):
        return "#imm"
    if re.fullmatch(r"\(.*,x\)", o):
        return "(zp,X)"
    if re.fullmatch(r"\(.*\),y", o):
        return "(zp),Y"
    if re.fullmatch(r"\(.*\)", o):
        return "(abs)"
    if mnemonic[0] == "b" and mnemonic in ("bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq"):
        return "rel"
    base = "zp" if length == 2 else "abs"
    if o.endswith(",x"):
        return base + ",X"
    if o.endswith(",y"):
        return base + ",Y"
    return base


def first_statement(text):
    """The first instruction or .byte line of da65's output: (mnemonic, operand)."""
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].rstrip()
        if not line.strip():
            continue
        if re.match(r"^\s*\w+\s*:?=", line):  # label := value
            continue
        line = re.sub(r"^[A-Za-z_@][\w@]*:", "", line).strip()  # leading label
        if not line:
            continue
        parts = line.split(None, 1)
        word = parts[0].lower()
        if word.startswith(".") and word not in (".byte", ".byt"):
            continue
        return word, parts[1].strip() if len(parts) > 1 else ""
    return None, None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    da65 = os.environ.get("DA65", "da65")
    forms = load_forms(sys.argv[1])
    mismatches, unknown, compared = [], [], 0
    with tempfile.TemporaryDirectory() as work:
        for f in forms:
            path = os.path.join(work, f"op{f['op']:02x}.bin")
            with open(path, "wb") as out:
                out.write(bytes([f["op"], 0x21, 0x43][:f["len"]]))
            r = subprocess.run([da65, "--cpu", "6502x", "--start-addr", "0x1000", path], capture_output=True, text=True)
            if r.returncode != 0:
                mismatches.append(f"{f['op']:02X}: da65 failed: {r.stderr.strip()}")
                continue
            word, opnd = first_statement(r.stdout)
            expect = DA65_NAMES.get(f["mn"], f["mn"].lower())
            if word in (".byte", ".byt", None):
                (mismatches if f["kind"] == "documented" else unknown).append(f"{f['op']:02X} {f['mn']} {f['mode']}")
                continue
            compared += 1
            got_mode = mode_of(word, opnd, f["len"])
            if word != expect or got_mode != f["mode"]:
                mismatches.append(f"{f['op']:02X}: ours {f['mn']} {f['mode']}, da65 '{word} {opnd}' ({got_mode})")
    print(f"decoder vs da65: {compared} opcodes compared, {len(mismatches)} mismatches, {len(unknown)} shown by da65 as .byte")
    if unknown:
        print("  .byte in da65: " + ", ".join(unknown))
    for m in mismatches:
        print("  MISMATCH " + m)
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
