"""End-to-end test of agent/m6510_annotate.py against a scripted mock of Ollama and the real m6510tool.

Run:  M6510TOOL=build/m6510tool python3 -m unittest discover -s tests/agent -v
"""
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mock_ollama import MockOllama  # noqa: E402

spec = importlib.util.spec_from_file_location("m6510_annotate", os.path.join(ROOT, "agent", "m6510_annotate.py"))
annotate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annotate)
annotate.Ollama.retry_delay = 0  # never wait between retries in tests

M6510TOOL = os.environ.get("M6510TOOL") or os.path.join(ROOT, "build", "m6510tool")

DEMO = """\
        * = $C000
start:  LDA #<msg
        LDY #>msg
        JSR print
        JSR copy8
        INC $D020
        INC counter
        RTS
print:  STA $FB
        STY $FC
        LDY #0
ploop:  LDA ($FB),Y
        BEQ pdone
        JSR $FFD2
        INY
        BNE ploop
pdone:  RTS
copy8:  LDX #7
c1:     LDA sprite,X
        STA $0400,X
        DEX
        BPL c1
        RTS
msg:    .BYTE "HELLO", 0
sprite: .BYTE 8, 5, 12, 12, 15, 32, 32, 33
counter:
        .BYTE 0
"""


def build(directory, source=DEMO, name="demo"):
    """Assembles a program to a PRG; returns (path, {symbol: address})."""
    src = os.path.join(directory, name + ".asm")
    out = os.path.join(directory, name + ".prg")
    sym = os.path.join(directory, name + ".sym")
    with open(src, "w") as f:
        f.write(source)
    r = subprocess.run([M6510TOOL, "asm", src, "-o", out, "-s", sym], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr + r.stdout
    symbols = {}
    with open(sym) as f:
        for line in f:
            parts = line.replace("=", " ").split()
            if len(parts) >= 2:
                symbols[parts[0]] = int(parts[1].lstrip("$"), 16)
    return out, symbols


def plans_for(sym):
    h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
    a = lambda n: f"0x{sym[n]:04X}"  # noqa: E731
    return {
        h("print"): [
            [("run_routine", {"addr": a("print"), "registers": {"A": f"0x{sym['msg'] & 0xFF:02X}", "Y": f"0x{sym['msg'] >> 8:02X}"}, "screenshot": True})],
            [("document_routine", {"addr": a("print"), "name": "A", "summary": "bad name on purpose"})],  # rejected: a register name
            [("document_routine", {"addr": a("print"), "name": "print_string", "summary": "Prints a zero-terminated PETSCII string.",
                                   "inputs": "A/Y = address of the text", "outputs": "none", "clobbers": "A Y", "confidence": "high"}),
             ("name_variable", {"addr": "0xFB", "name": "text_ptr", "size": 2, "comment": "the text being printed"}),
             ("name_label", {"addr": a("ploop"), "name": "next_char"}),
             ("name_label", {"addr": a("pdone"), "name": "print_done"}),
             ("add_comment", {"addr": a("ploop"), "text": "fetch the next character"}),
             ("add_comment", {"addr": f"0x{sym['ploop'] + 2:04X}", "text": "a zero byte ends the text"})],
            [("finish_routine", {"summary": "done"})],
            [("finish_routine", {})],
        ],
        h("copy8"): [
            [("run_routine", {"addr": a("copy8")})],
            [("name_label", {"addr": a("c1"), "name": "copy_loop"}),
             ("document_routine", {"addr": a("copy8"), "name": "show_title", "summary": "Copies 8 screen codes to the top of the screen.",
                                   "inputs": "none", "outputs": "none", "clobbers": "A X", "confidence": "medium"}),
             ("mark_data", {"addr": a("sprite"), "length": 8, "kind": "screen", "name": "title_text"}),
             ("name_variable", {"addr": "0x0400", "name": "screen_top", "size": 8}),
             ("add_comment", {"addr": a("copy8"), "text": "8 characters, X counts 7..0"}),
             ("add_comment", {"addr": a("c1"), "text": "one character per pass"})],
            [("finish_routine", {})],
            [("finish_routine", {})],
        ],
        h("start"): [
            [("run_routine", {"addr": a("start"), "max_instructions": 100000})],
            [("name_variable", {"addr": a("counter"), "name": "run_counter", "comment": "how often the demo ran"}),
             ("mark_data", {"addr": a("msg"), "kind": "text", "name": "greeting"}),
             ("add_comment", {"addr": a("start"), "text": "program entry"}),
             ("add_comment", {"addr": f"0x{sym['start'] + 7:04X}", "text": "then the title"}),
             ("document_routine", {"addr": a("start"), "name": "show_greeting_demo", "summary": "Prints a greeting, shows a title, flashes the border.",
                                   "inputs": "none", "outputs": "none", "clobbers": "A X Y", "confidence": "high"})],
            [("finish_routine", {})],
            [("finish_routine", {})],
        ],
    }


def run_main(argv):
    """Runs the orchestrator; returns (exit code, printed text)."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = annotate.main(argv)
    return code, buf.getvalue()


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M6510TOOL):
            raise unittest.SkipTest(f"m6510tool not found at {M6510TOOL} (set M6510TOOL)")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.prg, self.sym = build(self.dir)
        self.project = os.path.join(self.dir, "demo.proj.json")
        self.out = os.path.join(self.dir, "demo_annotated.asm")
        self.mocks = []

    def tearDown(self):
        for m in self.mocks:
            m.stop()
        self.tmp.cleanup()

    def mock(self, **kw):
        plans = kw.pop("plans", plans_for(self.sym))
        m = MockOllama(plans, **kw)
        self.url = m.start()
        self.mocks.append(m)
        return m

    def argv(self, *extra, model="mock"):
        return ["--prg", self.prg, "--entry", f"0x{self.sym['start']:04X}", "--host", self.url, "--model", model,
                "--project", self.project, "--out", self.out, "--m6510tool", M6510TOOL, "--max-turns", "10", *extra]


class OrchestratorTest(Base):
    def test_full_run_is_verified_and_annotated(self):
        mock = self.mock(capabilities_by_model={"mock": ["completion", "tools", "vision"]})
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertIn("VERIFIED", text)
        listing = read_text(self.out)
        for name in ("print_string", "show_title", "show_greeting_demo", "next_char", "print_done", "copy_loop", "run_counter", "text_ptr",
                     "greeting", "title_text", "screen_top"):
            self.assertIn(name, listing)
        self.assertIn("EXTCOL", listing)                   # a chip register, by name
        self.assertIn("CHROUT", listing)                   # a KERNAL entry, by name
        self.assertIn("; fetch the next character", listing)
        self.assertIn("confidence: medium", listing)
        self.assertIn("LDA #<greeting", listing)          # the pointer pair
        self.assertIn('.SCREEN "hello', listing)
        # the leaves come first: the two subroutines before the routine that calls them
        order = []
        for m in mock.requests_by_model["mock"]:
            a = m.split("ROUTINE_ADDR=")[1].split()[0].lower()
            if not order or order[-1] != a:
                order.append(a)
        self.assertEqual(order, [f"0x{self.sym['print']:04x}", f"0x{self.sym['copy8']:04x}", f"0x{self.sym['start']:04x}"])
        # the caller's task shows the callees by their real names and the KERNAL by name
        start_prompt = next(m for m in mock.requests_by_model["mock"] if f"ROUTINE_ADDR=0x{self.sym['start']:04X}" in m)
        self.assertIn("print_string", start_prompt)
        self.assertIn("show_title", start_prompt)
        print_prompt = next(m for m in mock.requests_by_model["mock"] if f"ROUTINE_ADDR=0x{self.sym['print']:04X}" in m)
        self.assertIn("CHROUT (0xFFD2, KERNAL)", print_prompt)
        self.assertIn("zero page used: 0xFB, 0xFC", print_prompt)
        # a rejected name came back as an error the model could read
        self.assertTrue(any("register name" in e for e in mock.tool_errors), mock.tool_errors)
        # run_routine reported memory, chip writes, KERNAL calls, text and the screen
        results = "\n".join(mock.tool_results)
        self.assertIn("registers after:", results)
        self.assertIn("memory changed", results)
        self.assertIn("EXTCOL", results)
        self.assertIn("returned: True", results)
        self.assertIn("text printed with CHROUT: 'HELLO'", results)
        self.assertIn("CHROUT A=48", results)
        self.assertIn("row  0: 'HELLO", results)
        self.assertIn("image attached", results)
        self.assertGreater(mock.images_seen, 0)            # the screenshot went to the model
        # the project file holds the annotations
        proj = json.loads(read_text(self.project))
        self.assertEqual({r["name"] for r in proj["routines"]}, {"print_string", "show_title", "show_greeting_demo"})

    def test_model_without_vision_gets_no_images(self):
        mock = self.mock(capabilities_by_model={"mock": ["completion", "tools"]})
        code, text = run_main(self.argv("--only", f"0x{self.sym['print']:04X}"))
        self.assertEqual(code, 0, text)
        self.assertIn("no vision", text)
        self.assertEqual(mock.images_seen, 0)
        self.assertIn("NOT shown", "\n".join(mock.tool_results))
        self.assertIn("You cannot see images", "\n".join(mock.system_prompts))

    def test_resume_skips_documented_routines(self):
        self.mock()
        code, _ = run_main(self.argv())
        self.assertEqual(code, 0)
        mock2 = self.mock()
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertEqual(mock2.requests, [])           # everything was documented already
        self.assertIn("project loaded", text)

    def test_sloppy_tool_calls(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['print']:04x}"
        a = f"0x{sym['print']:04X}"
        # calls written as text, arguments as a string, numbers as strings, $ addresses, unknown arguments, a wrong tool name
        plans[h] = [
            {"as_text": [("run_routine", {"addr": a, "registers": {"a": "$" + f"{sym['msg'] & 0xFF:02X}", "y": str(sym['msg'] >> 8)}})]},
            {"json_string": [("document_routine", {"addr": a, "name": "print_string", "summary": "s", "bogus": 1}),
                             ("name_label", {"addr": "$" + f"{sym['pdone']:04X}", "name": "print_done"})]},
            [("Functions.add_comment()", {"addr": a, "text": "a"}),
             ("add_comment", {"addr": f"{sym['ploop']:04X}", "text": "b"}),
             ("no_such_tool", {})],
            [("finish_routine", {})],
            [("finish_routine", {})],
        ]
        mock = self.mock(plans=plans)
        code, text = run_main(self.argv("--only", h))
        self.assertEqual(code, 0, text)
        results = "\n".join(mock.tool_results)
        self.assertIn("ignored unknown argument 'bogus'", results)
        self.assertIn("unknown tool 'no_such_tool'", results)
        self.assertIn("returned: True", results)
        self.assertIn("print_string", read_text(self.out))
        self.assertIn("print_done", read_text(self.out))

    def test_incomplete_routine_is_sent_back_once(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['print']:04x}"
        plans[h] = [
            [("document_routine", {"addr": f"0x{sym['print']:04X}", "name": "print_string", "summary": "s"})],
            [("finish_routine", {})],     # labels, variables, comments and the contract are missing: sent back
            [("name_label", {"addr": f"0x{sym['pdone']:04X}", "name": "print_done"}),
             ("add_comment", {"addr": f"0x{sym['print']:04X}", "text": "a"})],
            [("finish_routine", {})],
        ]
        mock = self.mock(plans=plans)
        run_main(self.argv("--only", h))
        results = "\n".join(mock.tool_results)
        self.assertIn("not finished yet", results)
        self.assertIn("labels without a real name", results)
        self.assertIn("memory locations used by number without a name: $FB, $FC", results)
        self.assertIn("comment(s)", results)
        self.assertIn("without inputs, outputs, clobbers", results)
        self.assertIn("print_done", read_text(self.out))               # the second finish_routine ended the routine

    def test_finish_before_documenting_is_refused(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['copy8']:04x}"
        plans[h] = [[("finish_routine", {})], [("finish_routine", {})]]
        mock = self.mock(plans=plans)
        code, text = run_main(self.argv("--only", h))
        self.assertTrue(any("document_routine" in e for e in mock.tool_errors))
        self.assertIn("failed", text)

    def test_invalid_tool_call_from_ollama_gets_a_hint(self):
        mock = self.mock(invalid_json_first=2)
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertEqual(mock.invalid_json_served, 2)
        self.assertIn("invalid tool call", text)
        self.assertTrue(mock.json_hints)

    def test_cut_off_answers_raise_the_limit(self):
        mock = self.mock(truncate_first=2)
        code, text = run_main(self.argv("--max-tokens", "1000"))
        self.assertEqual(code, 0, text)
        self.assertIn("cut off", text)
        self.assertTrue(mock.hints)
        self.assertGreater(max(o["num_predict"] for o in mock.options_seen), 1000)

    def test_model_without_thinking_is_asked_again(self):
        mock = self.mock(reject_think=True)
        code, text = run_main(self.argv("--think", "true"))
        self.assertEqual(code, 0, text)
        self.assertIn("rejected", mock.think_values)

    def test_one_failing_routine_does_not_end_the_run(self):
        mock = self.mock(crash_on_address=f"0x{self.sym['copy8']:04x}")
        code, text = run_main(self.argv())
        self.assertIn("model error on", text)
        self.assertIn("skipping this routine", text)
        proj = json.loads(read_text(self.project))
        self.assertEqual({r["name"] for r in proj["routines"]}, {"print_string", "show_greeting_demo"})
        self.assertIn("gaps left: 1 of 3", text)
        self.assertEqual(code, 0)   # the listing is still verified, only less annotated
        self.assertTrue(mock.requests)

    def test_configuration_errors(self):
        self.mock(capabilities_by_model={"nano": ["completion"]})
        code, _ = run_main(self.argv(model="nano"))
        self.assertEqual(code, 2)                      # no tool support
        self.mock(fail_with=404)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code, _ = run_main(self.argv())
        self.assertEqual(code, 2)                      # model not found: the message says what is installed and what to do
        self.assertIn("model 'mock' not found", err.getvalue())
        self.assertIn("Installed: other:1b, qwen3-vl:8b-instruct", err.getvalue())
        self.assertIn("ollama pull mock", err.getvalue())
        self.url = "http://127.0.0.1:9"
        with contextlib.redirect_stderr(io.StringIO()):
            code, _ = run_main(self.argv())
        self.assertEqual(code, 2)                      # nothing listens there

    def test_refinement_pass_only_gets_routines_with_gaps(self):
        plans = plans_for(self.sym)
        h = f"0x{self.sym['copy8']:04x}"
        del plans[h]                                   # the fast model gives up on copy8
        refine = {h: plans_for(self.sym)[h]}
        mock = self.mock(plans=plans, plans_by_model={"slow": refine})
        report = os.path.join(self.dir, "report.md")
        code, text = run_main(self.argv("--refine-model", "slow", "--report", report))
        self.assertEqual(code, 0, text)
        self.assertIn("== pass 2/2 (refine): model slow", text)
        asked = sorted({m.split("ROUTINE_ADDR=")[1].split()[0].lower() for m in mock.requests_by_model["slow"]})
        self.assertEqual(asked, [h])
        self.assertIn("Known gaps to close", mock.requests_by_model["slow"][0])
        self.assertIn("REFINEMENT PASS", "\n".join(mock.system_prompts))
        self.assertIn("show_title", read_text(self.out))
        self.assertIn("0 with gaps", read_text(report))

    def test_raw_binary_on_the_generic_machine(self):
        self.mock()
        raw = os.path.join(self.dir, "demo.bin")
        with open(self.prg, "rb") as f:
            data = f.read()[2:]
        with open(raw, "wb") as f:
            f.write(data)
        code, text = run_main(["--bin", raw, "--org", "0xC000", "--host", self.url, "--model", "mock", "--m6510tool", M6510TOOL,
                               "--machine", "generic", "--only", str(self.sym["print"]), "--max-turns", "10"])
        self.assertEqual(code, 0, text)
        self.assertIn("binary loaded: 0xC000", text)

    def test_dry_run(self):
        code, text = run_main(["--prg", self.prg, "--entry", f"0x{self.sym['start']:04X}", "--dry-run", "--m6510tool", M6510TOOL])
        self.assertEqual(code, 0, text)
        self.assertIn("== dry run", text)
        self.assertIn("3 routines, 3 still without documentation", text)
        self.assertIn("KERNAL calls: CHROUT x1", text)
        self.assertIn("chip registers used: EXTCOL", text)
        self.assertIn("2 data regions marked: 1 screen, 1 text", text)


EXAMPLE = os.path.join(ROOT, "examples", "c64", "hello_c64.asm")


class ExampleProgramTest(Base):
    """The walkthrough's example program: BASIC starter, jump tables, an interrupt handler (docs/walkthrough.md)."""

    def test_dry_run_of_the_example(self):
        r = subprocess.run([M6510TOOL, "asm", EXAMPLE, "-o", os.path.join(self.dir, "hello_c64.prg")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        code, text = run_main(["--prg", os.path.join(self.dir, "hello_c64.prg"), "--dry-run", "--m6510tool", M6510TOOL])
        self.assertEqual(code, 0, text)
        self.assertIn("program loaded: 0x0801-", text)
        self.assertIn("(10 SYS2061)", text)
        self.assertIn("10 routines", text)
        self.assertIn("jump table (rts)", text)
        self.assertIn("jump table (lohi)", text)
        self.assertIn("interrupt handler (0x0314)", text)
        self.assertIn("1 interrupt handler(s)", text)
        self.assertIn("1 screen, 1 sprites, 1 text", text)

    def test_interrupt_handler_runs_with_rti(self):
        r = subprocess.run([M6510TOOL, "asm", EXAMPLE, "-o", os.path.join(self.dir, "hello_c64.prg")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        server = annotate.M6510Server(M6510TOOL)
        try:
            args = annotate.build_parser().parse_args(["--prg", os.path.join(self.dir, "hello_c64.prg"), "--dry-run"])
            worker = annotate.Annotator(server, [], args)
            with redirect_stdout(io.StringIO()):
                worker.setup()
            irq = next(r for r in server.ok(cmd="routines")["routines"] if r.get("interrupt_vector"))
            result = worker.tools.t_run_routine(irq["addr"])
            self.assertIn("run as an interrupt handler", result)
            self.assertIn("returned: True", result)
            self.assertIn("BGCOL0", result)
            result = worker.tools.t_run_routine(irq["addr"], as_interrupt=False)
            self.assertIn("returned: False", result)
        finally:
            server.close()


class HelperTest(unittest.TestCase):
    def test_argument_coercion(self):
        clean, notes = annotate.prepare_args("run_routine", {"addr": "0xC000", "max_instructions": "1000", "keep_state": "yes", "x": 1})
        self.assertEqual(clean["max_instructions"], 1000)
        self.assertTrue(clean["keep_state"])
        self.assertTrue(notes)
        clean, _ = annotate.prepare_args("run_routine", {"addr": 49152, "registers": '{"A": 1}'})
        self.assertEqual(clean["addr"], 49152)
        self.assertEqual(clean["registers"], {"A": 1})
        with self.assertRaises(ValueError):
            annotate.prepare_args("run_routine", {})
        with self.assertRaises(ValueError):
            annotate.prepare_args("read_memory", {"addr": "0xC000", "length": "lots"})
        with self.assertRaises(ValueError):
            annotate.prepare_args("nothing", {})

    def test_address_normalisation(self):
        self.assertEqual(annotate.norm_addr("C000"), "0xC000")
        self.assertEqual(annotate.norm_addr("0801"), "0x0801")
        self.assertEqual(annotate.norm_addr("$FB"), "0xFB")
        self.assertEqual(annotate.norm_addr("1234"), "1234")          # decimal
        self.assertEqual(annotate.norm_addr("print_string"), "print_string")
        self.assertEqual(annotate.norm_addr(53280), 53280)
        with self.assertRaises(ValueError):
            annotate.norm_addr("")

    def test_tool_names_and_text_calls(self):
        self.assertEqual(annotate.clean_tool_name("functions.run_routine"), "run_routine")
        self.assertEqual(annotate.clean_tool_name("Run_Routine()"), "run_routine")
        calls = annotate.calls_from_content('<tool_call>{"name": "screenshot", "arguments": {}}</tool_call>')
        self.assertEqual(calls[0]["function"]["name"], "screenshot")
        self.assertEqual(annotate.calls_from_content("just text"), [])

    def test_compaction_keeps_the_task_and_drops_old_images(self):
        msgs = [{"role": "system", "content": "s"}, {"role": "user", "content": "task"}]
        for i in range(5):
            msgs.append({"role": "tool", "content": "x" * 1000})
            msgs.append({"role": "user", "content": "shot", "images": ["AAAA"]})
        annotate.compact_messages(msgs, 500)
        self.assertEqual(msgs[1]["content"], "task")
        self.assertEqual(sum(1 for m in msgs if m.get("images")), 2)
        self.assertIn("removed", msgs[2]["content"])

    def test_petscii_preview(self):
        self.assertEqual(annotate.petscii_of("48454C4C4F"), ' "HELLO"')
        self.assertEqual(annotate.petscii_of("C8C5"), ' "he"')
        self.assertEqual(annotate.petscii_of("0001020304"), "")


if __name__ == "__main__":
    unittest.main()
