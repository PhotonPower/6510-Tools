"""Self-extracting programs (docs/unpacking.md): a packer in Python, a depacker in 6510 assembler, the real m6510tool.

run_until_unpacked must stop where the program jumps into the code it has just written, save_memory must give back exactly
the original program, a two-stage packer is followed with continue:true, and the packed program also comes out of a D64.

Run:  M6510TOOL=build/m6510tool python3 -m unittest discover -s tests/agent -v
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "unpack"))
import lzpack  # noqa: E402

M6510TOOL = os.environ.get("M6510TOOL") or os.path.join(ROOT, "build", "m6510tool")
DEPACK = os.path.join(HERE, "unpack", "depack.asm")

PAYLOAD = """\
        * = $C000
entry:  LDX #0
print:  LDA text,X
        BEQ done
        JSR $FFD2
        INX
        BNE print
done:   RTS
text:   .BYTE "UNPACKED AND RUNNING", 13, 0
table:
"""


def table_lines(n):
    """Data that compresses (repetitions) but is not trivial."""
    lines = []
    for i in range(n):
        lines.append("        .BYTE " + ",".join(str((i * 3 + k * (i % 7)) & 0xFF) for k in range(16)))
    return "\n".join(lines) + "\n"


def assemble(directory, name, source):
    src = os.path.join(directory, name + ".asm")
    out = os.path.join(directory, name + ".prg")
    with open(src, "w") as f:
        f.write(source)
    r = subprocess.run([M6510TOOL, "asm", src, "-o", out], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    with open(out, "rb") as f:
        data = f.read()
    return out, data[0] | data[1] << 8, data[2:]


def packed_program(directory, name, payload, dest, entry, origin, basic):
    """The depacker with the packed payload behind it, assembled."""
    packed = lzpack.pack(payload)
    assert lzpack.unpack(packed) == payload
    with open(DEPACK) as f:
        depack = f.read()
    head = f"DEST = ${dest:04X}\nENTRY = ${entry:04X}\nORIGIN = ${origin:04X}\n"
    if basic:
        head += "        * = $0801\n        .BASIC 10, start\n"
    rows = [packed[i:i + 16] for i in range(0, len(packed), 16)]
    data = "".join("        .BYTE " + ",".join(str(b) for b in row) + "\n" for row in rows)
    return assemble(directory, name, head + depack + data), len(packed)


def d64_with(name, prg):
    """A minimal 35-track D64 with one PRG file (track 1 upwards, directory on 18/1)."""
    spt = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5
    offset = lambda t, s: (sum(spt[:t - 1]) + s) * 256  # noqa: E731
    img = bytearray(683 * 256)
    chunks = [prg[i:i + 254] for i in range(0, len(prg), 254)]
    sectors = [(1 + i // 21, i % 21) for i in range(len(chunks))]
    for i, chunk in enumerate(chunks):
        o = offset(*sectors[i])
        if i + 1 < len(chunks):
            img[o], img[o + 1] = sectors[i + 1]
        else:
            img[o], img[o + 1] = 0, len(chunk) + 1
        img[o + 2:o + 2 + len(chunk)] = chunk
    bam = offset(18, 0)
    img[bam:bam + 3] = bytes((18, 1, 0x41))
    img[bam + 0x90:bam + 0xA0] = b"PACKED TEST".ljust(16, b"\xA0")
    img[bam + 0xA2:bam + 0xA7] = b"PT\xA02A"
    d = offset(18, 1)
    img[d:d + 2] = bytes((0, 0xFF))
    img[d + 2] = 0x82
    img[d + 3], img[d + 4] = sectors[0]
    img[d + 5:d + 21] = name.encode().ljust(16, b"\xA0")
    img[d + 30] = len(chunks)
    return bytes(img)


class Server:
    def __init__(self):
        self.proc = subprocess.Popen([M6510TOOL, "serve", "--allow-files"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def __call__(self, **req):
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)
        self.proc.stdout.close()


class UnpackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M6510TOOL):
            raise unittest.SkipTest(f"m6510tool not found at {M6510TOOL} (set M6510TOOL)")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        _, self.base, self.payload = assemble(self.dir, "payload", PAYLOAD + table_lines(200))  # ends below the I/O area at $D000
        self.assertEqual(self.base, 0xC000)
        self.s = Server()

    def tearDown(self):
        self.s.close()
        self.tmp.cleanup()

    def test_one_stage(self):
        (prg, _, _), packed_len = packed_program(self.dir, "sfx", self.payload, 0xC000, 0xC000, 0x080D, basic=True)
        self.assertLess(packed_len, len(self.payload))
        s = self.s
        s(cmd="machine", type="c64")
        r = s(cmd="load_file", file=prg)
        self.assertEqual(r["entry"], "0x080D")
        r = s(cmd="run_until_unpacked", min_written=1024)
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["stop"], "unpacked")
        self.assertEqual(r["pc"], "0xC000")
        self.assertEqual(r["region"]["start"], "0xC000")
        self.assertEqual(r["region"]["size"], len(self.payload))
        self.assertGreaterEqual(r["written_bytes"], len(self.payload))
        out = os.path.join(self.dir, "unpacked.prg")
        r = s(cmd="save_memory", addr="0xC000", len=len(self.payload), file=out)
        self.assertTrue(r["ok"], r)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), bytes((0x00, 0xC0)) + self.payload)
        # the unpacked program runs and can be analysed
        r = s(cmd="run")
        self.assertEqual(s(cmd="console")["text"], "UNPACKED AND RUNNING\n")
        r = s(cmd="analyze", entries=["0xC000"], **{"from": "0xC000", "to": f"0x{0xC000 + len(self.payload):X}"})
        self.assertTrue(r["ok"], r)
        self.assertTrue(s(cmd="listing")["verified"])
        # written: the ranges, and switched off
        w = s(cmd="written", min_length=1000)
        self.assertEqual(w["ranges"][0]["start"], "0xC000")
        self.assertFalse(s(cmd="track_writes", on=False)["tracking"])

    def test_two_stages_with_continue(self):
        # stage 2: a depacker at $9000 for the payload; stage 1 (at $0801) unpacks stage 2 to $9000
        (_, base2, stage2), _ = packed_program(self.dir, "stage2", self.payload, 0x4000, 0x4000, 0x9000, basic=False)
        self.assertEqual(base2, 0x9000)
        (prg, _, _), _ = packed_program(self.dir, "sfx2", stage2, 0x9000, 0x9000, 0x080D, basic=True)
        s = self.s
        s(cmd="machine", type="c64")
        s(cmd="load_file", file=prg)
        r = s(cmd="run_until_unpacked", min_written=256)
        self.assertEqual((r["stop"], r["pc"]), ("unpacked", "0x9000"))
        r = s(cmd="run_until_unpacked", min_written=256, **{"continue": True})
        self.assertEqual((r["stop"], r["pc"]), ("unpacked", "0x4000"))
        self.assertEqual(r["region"]["size"], len(self.payload))
        r = s(cmd="read", addr="0x4000", len=16)
        self.assertEqual(bytes.fromhex(r["hex"].replace(" ", "")), self.payload[:16])

    def test_ignore_and_limits(self):
        (prg, _, _), _ = packed_program(self.dir, "sfx", self.payload, 0xC000, 0xC000, 0x080D, basic=True)
        s = self.s
        s(cmd="machine", type="c64")
        s(cmd="load_file", file=prg)
        r = s(cmd="run_until_unpacked", min_written=1024, ignore=[["0xC000", "0xD000"]], max_instructions=2000000)
        self.assertNotEqual(r["stop"], "unpacked")     # the only result is ignored: the program runs on (to its end)
        s(cmd="load_file", file=prg)
        r = s(cmd="run_until_unpacked", max_instructions=100)
        self.assertEqual(r["stop"], "instruction_limit")
        for bad in ({"min_written": 0}, {"ignore": "x"}, {"ignore": [["0x1000"]]}):
            self.assertFalse(s(cmd="run_until_unpacked", **bad)["ok"])
        self.assertFalse(s(cmd="save_memory", addr="0xFFFF", len=2, file=os.path.join(self.dir, "x"))["ok"])
        self.assertFalse(s(cmd="save_memory", addr="0x1000", len=2, file=os.path.join(self.dir, "x"), format="hex")["ok"])

    def test_out_of_a_d64(self):
        (prg, _, _), _ = packed_program(self.dir, "sfx", self.payload, 0xC000, 0xC000, 0x080D, basic=True)
        with open(prg, "rb") as f:
            data = f.read()
        disk = os.path.join(self.dir, "packed.d64")
        with open(disk, "wb") as f:
            f.write(d64_with("PACKED", data))
        r = subprocess.run([M6510TOOL, "d64", "ls", disk], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"PACKED"', r.stdout)
        out = os.path.join(self.dir, "x.prg")
        r = subprocess.run([M6510TOOL, "d64", "extract", disk, "PACK*", "-o", out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), data)
        s = self.s
        s(cmd="machine", type="c64")
        r = s(cmd="load_file", file=disk, name="PACKED")
        self.assertEqual(r["entry"], "0x080D", r)
        r = s(cmd="run_until_unpacked", min_written=1024)
        self.assertEqual(r["pc"], "0xC000")


if __name__ == "__main__":
    unittest.main()
