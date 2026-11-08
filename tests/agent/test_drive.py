"""agent/m6510_drive.py: the annotation tools driven by hand, one command per process, the state in a work directory.

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
M6510TOOL = os.environ.get("M6510TOOL") or os.path.join(ROOT, "build", "m6510tool")
DRIVE = os.path.join(ROOT, "agent", "m6510_drive.py")
SOURCE = os.path.join(ROOT, "examples", "c64", "hello_c64.asm")


class DriveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M6510TOOL):
            raise unittest.SkipTest(f"m6510tool not found at {M6510TOOL} (set M6510TOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.prg = os.path.join(cls.tmp.name, "hello_c64.prg")
        r = subprocess.run([M6510TOOL, "asm", SOURCE, "-o", cls.prg], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr + r.stdout
        cls.work = os.path.join(cls.tmp.name, "work")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args, work=None, expect=0):
        r = subprocess.run([sys.executable, DRIVE, "--work", work or self.work, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, f"{args}: {r.stdout}\n{r.stderr}")
        return r.stdout, r.stderr

    def test_a_whole_session(self):
        """init -> status -> show -> run_routine -> annotate -> finish -> listing: what a model does in a loop, one command per process."""
        out, _ = self.drive("init", "--fresh", "--prg", self.prg, "--m6510tool", M6510TOOL, "--lang", "Deutsch")
        self.assertIn("10 routines", out)
        self.assertIn("(10 SYS2061)", out)
        out, _ = self.drive("status")
        self.assertIn("10 routines, processing order", out)
        self.assertIn("0 of 10 finished.", out)
        self.assertIn("Next: `show 0x082D`", out)

        out, _ = self.drive("show", "0x082D")
        self.assertIn("ROUTINE_ADDR=0x082D", out)
        self.assertIn("CHROUT (0xFFD2, KERNAL)", out)
        self.assertIn("LDA ($FB),Y                     ; $0833", out)
        self.assertNotIn(".CPU", out)                                          # no prelude in the excerpt a model reads
        out_main, _ = self.drive("show", "0x080D")
        self.assertIn("Referenced from this routine", out_main)                # what the operands point at, with the contents
        self.assertIn('(text, 13 bytes): "HELLO, C64!{RETURN}"', out_main)
        out, _ = self.drive("show", "0x0898")
        self.assertIn("interrupt handler installed through the vector at 0x0314", out)
        self.assertIn("writes BGCOL0", out)
        out, _ = self.drive("show", "0x0854")
        self.assertIn("reached through a jump table", out)
        out, _ = self.drive("show", "0x082D", "--system")
        self.assertIn("expert 6510 (6502) and Commodore 64 reverse engineer", out)

        out, _ = self.drive("tool", "run_routine", json.dumps({"addr": "0x082D", "memory": [{"addr": "0xC000", "text": "HI"}],
                                                               "registers": {"A": "0x00", "Y": "0xC0"}}))
        self.assertIn("stop: returned  returned: True", out)
        self.assertIn("text printed with CHROUT: 'HI'", out)

        # annotations survive the end of the process
        self.drive("tool", "document_routine", json.dumps({"addr": "0x082D", "name": "print_string", "summary": "Prints a zero-terminated string.",
                                                           "inputs": "A/Y = address", "confidence": "high"}))
        # the completeness check sends the model back (placeholder labels, numbered variables, no comments) ...
        out, _ = self.drive("finish", "0x082D")
        self.assertIn("not finished yet", out)
        self.assertIn("labels without a real name", out)
        self.assertIn("memory locations used by number", out)
        self.assertIn("document_routine without outputs, clobbers", out)
        with open(os.path.join(self.work, "state.json")) as f:
            self.assertEqual(json.load(f)["warned"], ["0x082D"])
        for name, addr in (("next_char", "0x0833"), ("print_done", "0x083D")):
            self.drive("tool", "name_label", json.dumps({"addr": addr, "name": name}))
        self.drive("tool", "name_variable", json.dumps({"addr": "0xFB", "name": "text_ptr", "size": 2}))
        self.drive("tool", "add_comment", json.dumps({"addr": "0x082D", "text": "remember the address of the text"}))
        self.drive("tool", "add_comment", json.dumps({"addr": "0x0835", "text": "a zero byte ends it"}))
        # ... and a second call does NOT pass while something is missing
        out, _ = self.drive("finish", "0x082D")
        self.assertIn("not finished yet", out)
        self.assertIn("document_routine without outputs, clobbers", out)
        self.assertNotIn("labels without a real name", out)
        self.drive("tool", "document_routine", json.dumps({"addr": "0x082D", "name": "print_string", "summary": "Prints a zero-terminated string.",
                                                           "inputs": "A/Y = address", "outputs": "none", "clobbers": "A Y", "confidence": "high"}))
        out, _ = self.drive("finish", "0x082D")
        self.assertIn("ok: routine finished", out)
        out, _ = self.drive("status")
        self.assertIn("1 of 10 finished.", out)
        self.assertIn("finished, confidence high", out)
        self.assertIn("Next: `show 0x0854`", out)
        out, _ = self.drive("report")
        self.assertIn("9 of 10 routines are not finished or still have gaps.", out)

        out, _ = self.drive("listing", "--out", os.path.join(self.tmp.name, "out.asm"))
        self.assertIn("VERIFIED", out)
        with open(os.path.join(self.tmp.name, "out.asm")) as f:
            text = f.read()
        self.assertIn("print_string:", text)
        self.assertIn("next_char:", text)
        self.assertIn("LDA (text_ptr),Y", text)
        self.assertIn("; remember the address of the text", text)
        self.assertIn(".BASIC 10,sub_080D", text)

    def test_accept_gaps_is_the_orchestrators_rule(self):
        """With --accept-gaps the first finish may send you back and the second is final."""
        work = os.path.join(self.tmp.name, "work_gaps")
        self.drive("init", "--fresh", "--prg", self.prg, "--m6510tool", M6510TOOL, work=work)
        self.drive("tool", "document_routine", json.dumps({"addr": "0x085A", "name": "count_up", "summary": "x"}), work=work)
        out, _ = self.drive("finish", "0x085A", "--accept-gaps", work=work)
        self.assertIn("not finished yet", out)
        self.assertIn("a second call is accepted as final", out)
        out, _ = self.drive("finish", "0x085A", "--accept-gaps", work=work)
        self.assertIn("ok: routine finished", out)
        out, _ = self.drive("finish", "0x085A", work=work)                      # the strict rule still sees what is missing
        self.assertIn("not finished yet", out)

    def test_errors_are_plain(self):
        work = os.path.join(self.tmp.name, "empty")
        _, err = self.drive("status", work=work, expect=1)
        self.assertIn("run `init` first", err)
        _, err = self.drive("init", work=os.path.join(self.tmp.name, "w2"), expect=1)
        self.assertIn("init needs the program", err)
        _, err = self.drive("init", "--prg", self.prg, "--project", "x.json", work=os.path.join(self.tmp.name, "w3"), expect=1)
        self.assertIn("--project is set by the work directory", err)
        r = subprocess.run([sys.executable, DRIVE, "--work", os.path.join(self.tmp.name, "w4"), "init", "--prg", self.prg, "--bogus"], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)                                   # an option nobody knows is refused right away
        self.drive("init", "--fresh", "--prg", self.prg, "--m6510tool", M6510TOOL, work=os.path.join(self.tmp.name, "w5"))
        w5 = os.path.join(self.tmp.name, "w5")
        _, err = self.drive("show", "0x9999", work=w5, expect=1)
        self.assertIn("no routine at 0x9999", err)
        _, err = self.drive("tool", "run_routine", "{not json", work=w5, expect=1)
        self.assertIn("not valid JSON", err)
        out, _ = self.drive("tool", "no_such_tool", "{}", work=w5, expect=1)
        self.assertIn("unknown tool", out)
        _, err = self.drive("tool", "finish_routine", "{}", work=w5, expect=1)
        self.assertIn("needs the routine", err)
        out, _ = self.drive("tool", "document_routine", json.dumps({"addr": "0x082D", "name": "X", "summary": "x"}), work=w5, expect=1)
        self.assertIn("error", out)                                           # a register name is not a name
        out, _ = self.drive("finish", "0x082D", work=w5, expect=1)
        self.assertIn("document_routine for this routine first", out)         # not documented yet


if __name__ == "__main__":
    unittest.main()
