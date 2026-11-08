#!/usr/bin/env python3
"""Annotates a Commodore 64 / 6510 program with the help of a local LLM (Ollama, e.g. Qwen3-VL).

The model never edits code. It works one subroutine at a time and may

  * read the annotated listing, disassemble and read memory,
  * RUN the routine in the emulator (with chosen registers / memory) and look at what changed:
    registers, memory, writes to VIC/SID/CIA registers, KERNAL calls, text printed with CHROUT and the
    screen (as text, and as an image for models that can see),
  * give names and comments (labels, routine documentation, variables, data regions).

Everything the model says is stored as names and comments only; the final listing is generated from
the real bytes and verified by assembling it again, so it always describes exactly the same program.

Requires only the Python standard library, a built `m6510tool`, and a running Ollama server with a
tool-capable model (default `qwen3-vl:8b-instruct`, as in the sister projects; `ollama list` shows what is installed).

Example:
    python3 agent/m6510_annotate.py --prg game.prg --project game.m6510proj.json --out game_annotated.asm --lang German
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections import Counter

# --------------------------------------------------------------------------------------------
# talking to the emulator


class ServerError(RuntimeError):
    pass


class M6510Server:
    """`m6510tool serve` as a subprocess: one JSON line in, one JSON line out."""

    def __init__(self, exe, allow_files=True):
        cmd = [exe, "serve"] + (["--allow-files"] if allow_files else [])
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
                                     errors="replace", bufsize=1)

    def call(self, **request):
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise ServerError("m6510tool serve terminated unexpectedly")
        return json.loads(line)

    def ok(self, **request):
        """Like call(), but raises on {"ok": false}."""
        reply = self.call(**request)
        if not reply.get("ok"):
            raise ServerError(reply.get("error", "unknown error") + (" " + json.dumps(reply["errors"]) if "errors" in reply else ""))
        return reply

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        finally:
            self.proc.stdout.close()


# --------------------------------------------------------------------------------------------
# talking to Ollama


class OllamaError(RuntimeError):
    pass


class OllamaFormatError(OllamaError):
    """Ollama could not parse what the model wrote as a tool call (usually invalid JSON, e.g. a bare 0xC000 instead of "0xC000")."""


class Ollama:
    retry_delay = 2  # seconds, multiplied by the attempt number
    max_tokens_limit = 16384  # automatic escalation stops here
    vision = True  # set from capabilities(): False means screenshots cannot be shown to this model

    def __init__(self, host, model, timeout=900, think=False, num_ctx=32768, temperature=0.7, max_tokens=4096):
        self.url = host.rstrip("/") + "/api/chat"
        self.model, self.timeout, self.think = model, timeout, think
        # Qwen recommends sampling (not greedy / low temperature: that makes it repeat itself endlessly);
        # num_predict caps one answer so that a runaway "thinking" cannot eat the whole context window.
        self.options = {"num_ctx": num_ctx, "temperature": temperature, "top_p": 0.8, "top_k": 20, "presence_penalty": 1.0,
                        "num_predict": max_tokens}

    def installed(self):
        """Names of the installed models (best effort, for error messages), or None."""
        try:
            with urllib.request.urlopen(self.url.rsplit("/api/", 1)[0] + "/api/tags", timeout=10) as resp:
                return [m.get("name", "?") for m in json.loads(resp.read()).get("models", [])]
        except Exception:  # noqa: BLE001
            return None

    def capabilities(self):
        """What Ollama says the model can do (e.g. {"completion", "vision", "tools", "thinking"}), or None if unknown."""
        try:
            req = urllib.request.Request(self.url.rsplit("/api/", 1)[0] + "/api/show", data=json.dumps({"model": self.model}).encode(),
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=30) as resp:
                caps = json.loads(resp.read()).get("capabilities")
            return set(caps) if isinstance(caps, list) else None
        except Exception:  # noqa: BLE001  (older Ollama versions, unreachable server: assume the model is fine)
            return None

    def chat(self, messages, tools):
        body = {"model": self.model, "messages": messages, "tools": tools, "stream": False, "options": self.options}
        if self.think is not None:
            body["think"] = self.think
        data = json.dumps(body).encode()
        last = None
        for attempt in range(3):
            try:
                req = urllib.request.Request(self.url, data=data, headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    return json.loads(resp.read())
            except urllib.error.HTTPError as e:  # the server answered: do not retry client errors
                detail = e.read().decode(errors="replace")[:500]
                if e.code == 500 and re.search(r"invalid character|unexpected end of JSON|cannot unmarshal|error parsing tool call|invalid JSON", detail, re.I):
                    raise OllamaFormatError(detail) from e  # the model's fault, not the server's: retrying the same request is pointless
                if e.code == 400 and "think" in detail.lower() and "think" in body:
                    # the model has no thinking mode: ask again without the parameter, and remember
                    self.think = None
                    del body["think"]
                    data = json.dumps(body).encode()
                    last = OllamaError(detail)
                    continue
                if e.code == 404:
                    names = self.installed()
                    raise OllamaError(f"model '{self.model}' not found on {self.url.rsplit('/api/', 1)[0]} (Ollama said: {detail.strip()[:120]}). "
                                      + ("Installed: " + (", ".join(names) if names else "nothing") + ". " if names is not None else "")
                                      + f"Pull it with `ollama pull {self.model}` or choose another one with --model.") from e
                if 400 <= e.code < 500:
                    raise OllamaError(f"Ollama returned HTTP {e.code}: {detail}") from e
                last = OllamaError(f"Ollama returned HTTP {e.code}: {detail}")
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                last = OllamaError(f"cannot reach Ollama at {self.url}: {e}")
            time.sleep(self.retry_delay * (attempt + 1))
        raise last


def check_server(server):
    """The orchestrator relies on the reverse-engineering commands of the C++ side; an old build would fail in confusing ways."""
    reply = server.call(cmd="help")
    cmds = reply.get("commands", []) if reply.get("ok") else []
    missing = [c for c in ("analyze", "routines", "listing", "ann_routine", "call", "checkpoint", "screenshot") if c not in cmds]
    if missing:
        raise ServerError("your m6510tool is outdated (it lacks: " + ", ".join(missing) + "). Rebuild it:  cmake --build build   (after git pull)")


# --------------------------------------------------------------------------------------------
# tool definitions (what the model sees)


def _fn(name, description, properties, required=()):
    return {"type": "function", "function": {"name": name, "description": description,
                                              "parameters": {"type": "object", "properties": properties, "required": list(required)}}}


ADDR = {"type": "string", "description": 'Address as a hexadecimal string such as "0xC000", or a label name. Plain numbers in JSON are decimal.'}
DATA_KINDS = ["bytes", "words", "text", "screen", "pointers", "rts_pointers", "lohi", "sprites", "charset"]

TOOLS = [
    _fn("get_listing", "Annotated listing of an address range, as currently named and commented.",
        {"from": ADDR, "to": ADDR}, ["from", "to"]),
    _fn("disassemble", "Disassemble `count` instructions starting at an address.",
        {"addr": ADDR, "count": {"type": "integer"}}, ["addr"]),
    _fn("read_memory", "Hex dump of memory (with the bytes read as PETSCII and as screen codes).",
        {"addr": ADDR, "length": {"type": "integer"}}, ["addr"]),
    _fn("run_routine",
        "Run the routine at `addr` in the emulator like a JSR and report what it did: registers afterwards, memory it changed, writes to "
        "VIC/SID/CIA registers, KERNAL calls, text printed with CHROUT and changes of the screen. The machine is restored afterwards unless "
        "keep_state is true. Use this to TEST your hypotheses instead of guessing. `setup` routines run first (e.g. the program's initialisation).",
        {"addr": ADDR,
         "registers": {"type": "object", "description": 'Initial registers, e.g. {"A": 65, "X": "0x10", "Y": 0, "P": "0x00"}'},
         "memory": {"type": "array", "description": 'Memory to set up first, e.g. a zero-page pointer and the text it points to: '
                                                    '[{"addr": "0xFB", "hex": "00C1"}, {"addr": "0xC100", "text": "HELLO"}] '
                                                    '(text: a zero byte is added unless "nul" is false)',
                    "items": {"type": "object", "properties": {"addr": ADDR, "hex": {"type": "string"}, "text": {"type": "string"}, "nul": {"type": "boolean"}}}},
         "setup": {"type": "array", "description": 'Routines to run FIRST on the same machine, e.g. the initialisation of the program: [{"addr": "0x080D"}] '
                                                  '(optional "registers"). Their output and memory changes are not part of the report.',
                   "items": {"type": "object", "properties": {"addr": ADDR, "registers": {"type": "object"}}}},
         "max_instructions": {"type": "integer", "description": "default 200000 (also the limit for each setup routine)"},
         "screenshot": {"type": "boolean", "description": "attach an image of the screen (zoomed to the changed area) after the run"},
         "as_interrupt": {"type": "boolean", "description": "run it like an interrupt handler (return with RTI; default: true for the handlers the analysis found)"},
         "keep_state": {"type": "boolean", "description": "keep the changes (then call reset_machine when done)"}},
        ["addr"]),
    _fn("screenshot", "Image of the current screen. Optional crop box in text cells (40 columns x 25 rows) and zoom.",
        {"col0": {"type": "integer"}, "row0": {"type": "integer"}, "col1": {"type": "integer"}, "row1": {"type": "integer"},
         "zoom": {"type": "integer"}}),
    _fn("remove_annotation", "Remove what you annotated at an address: its name, documentation, variable, data region and comment (e.g. to undo a wrong mark_data or name).",
        {"addr": ADDR}, ["addr"]),
    _fn("reset_machine", "Restore the machine to the state it had when the analysis started.", {}),
    _fn("name_label", "Give an address (code or data) a meaningful name, e.g. a loop or entry point.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case, ASCII, unique"}}, ["addr", "name"]),
    _fn("document_routine", "Name and document the subroutine. Required before finish_routine.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case verb phrase, e.g. print_string"},
         "summary": {"type": "string", "description": "what it does, 1-3 sentences"},
         "inputs": {"type": "string", "description": 'registers/memory read, e.g. "A = character, X = column, ($FB) = text"'},
         "outputs": {"type": "string", "description": "results and return values, flags (e.g. C = 1 on error)"},
         "clobbers": {"type": "string", "description": 'registers destroyed, e.g. "A Y"'},
         "confidence": {"type": "string", "enum": ["high", "medium", "low"],
                        "description": "how sure you are: high = verified by running it, medium = plausible from the code, low = a guess"}},
        ["addr", "name", "summary"]),
    _fn("name_variable", "Name a memory location the code reads/writes (variable, flag, counter, zero-page pointer: size 2, buffer).",
        {"addr": ADDR, "name": {"type": "string"}, "size": {"type": "integer", "description": "bytes, default 1 (2 for a pointer)"},
         "comment": {"type": "string", "description": "meaning and unit"}}, ["addr", "name"]),
    _fn("mark_data", "Declare a range as data (not code) and how to display it. For kind text the length may be left out: the string ends with its zero byte. "
                     "lohi: a table of low bytes at addr and of high bytes at `partner` (length = entries; minus_one for RTS-trick tables).",
        {"addr": ADDR, "length": {"type": "integer", "description": "bytes; optional for kind text"},
         "kind": {"type": "string", "enum": DATA_KINDS}, "partner": ADDR, "minus_one": {"type": "boolean"},
         "name": {"type": "string"}, "comment": {"type": "string"}}, ["addr", "kind"]),
    _fn("add_comment", "Comment for one line of code or data (appears behind the instruction or .BYTE line), or a block comment above it. Use the exact "
                       "address from the right-hand comment of the listing line; an address inside an instruction is refused.",
        {"addr": ADDR, "text": {"type": "string"}, "block": {"type": "boolean"}}, ["addr", "text"]),
    _fn("mark_constant", "Mark the instruction at an address as one whose immediate operand is a plain NUMBER, not half of an address: the listing then "
                         "writes LDA #$10 instead of LDA #<label. Use the address of the instruction itself; remove=true takes the mark back.",
        {"addr": ADDR, "remove": {"type": "boolean"}}, ["addr"]),
    _fn("finish_routine", "Call when the routine is fully documented and commented.", {"summary": {"type": "string"}}),
]

# --------------------------------------------------------------------------------------------
# helpers


def _coerce(value, schema):
    """Converts a model-supplied value to the JSON type the schema asks for (models are sloppy about this)."""
    kind = schema.get("type")
    if kind == "boolean":
        if isinstance(value, str):
            low = value.strip().lower()
            if low in ("true", "yes", "1"):
                return True
            if low in ("false", "no", "0", ""):
                return False
            raise ValueError(f"expected true or false, got {value!r}")
        return bool(value)
    if kind == "integer":
        if isinstance(value, bool):
            raise ValueError("expected a number")
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, str):
            try:
                return int(value.strip().replace("$", "0x"), 0)
            except ValueError:
                raise ValueError(f"expected a number, got {value!r}") from None
        if isinstance(value, int):
            return value
        raise ValueError(f"expected a number, got {value!r}")
    if kind in ("object", "array"):
        if isinstance(value, str):  # JSON written as a string
            try:
                value = json.loads(value)
            except json.JSONDecodeError:
                raise ValueError(f"expected {kind} (JSON), got text") from None
        if kind == "object" and not isinstance(value, dict) or kind == "array" and not isinstance(value, list):
            raise ValueError(f"expected {kind}")
        return value
    if kind == "string":
        if schema is ADDR and isinstance(value, (int, float)) and not isinstance(value, bool):
            return int(value)  # a JSON number is a decimal address; it must not turn into a "hex" string
        return value if isinstance(value, str) else str(value)
    return value


def prepare_args(tool_name, args):
    """Validates and normalises tool arguments against the tool's schema.

    Returns (clean_args, notes). Unknown arguments are dropped with a note; wrong types are converted when
    that is unambiguous. Raises ValueError with a message the model can act on.
    """
    spec = next((t["function"] for t in TOOLS if t["function"]["name"] == tool_name), None)
    if spec is None:
        raise ValueError(f"unknown tool '{tool_name}'. Available tools: " + ", ".join(t["function"]["name"] for t in TOOLS))
    props = spec["parameters"]["properties"]
    required = spec["parameters"].get("required", [])
    clean, notes = {}, []
    for key, value in args.items():
        if key not in props:
            notes.append(f"ignored unknown argument '{key}' (valid: {', '.join(props) or 'none'})")
            continue
        if value is None:
            continue
        try:
            clean[key] = _coerce(value, props[key])
        except ValueError as e:
            raise ValueError(f"argument '{key}': {e}") from None
    missing = [k for k in required if k not in clean]
    if missing:
        raise ValueError(f"missing required argument(s): {', '.join(missing)}. Valid arguments: {', '.join(props)}")
    return clean, notes


def clean_tool_name(name):
    """'functions.run_routine', 'run_routine()' and 'Run_Routine' all mean run_routine."""
    name = (name or "").strip().rstrip("()").split(".")[-1]
    known = {t["function"]["name"] for t in TOOLS}
    if name in known:
        return name
    return name.lower() if name.lower() in known else name


_TOOL_CALL_TAG = re.compile(r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)", re.S)


def calls_from_content(content):
    """Some chat templates make the model write the call as text instead of using `tool_calls`.

    Understands <tool_call>{"name": ..., "arguments": {...}}</tool_call> and a bare JSON object (optionally in a
    ```json fence) with the same keys. Returns a list of {"function": {...}} dicts (empty if there is none).
    """
    if not content:
        return []
    candidates = _TOOL_CALL_TAG.findall(content)
    if not candidates:
        fenced = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", content, re.S)
        candidates = fenced or ([content.strip()] if content.strip().startswith("{") else [])
    calls = []
    for text in candidates:
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            continue
        for item in obj if isinstance(obj, list) else [obj]:
            if isinstance(item, dict) and isinstance(item.get("name"), str) and "arguments" in item:
                calls.append({"function": {"name": item["name"], "arguments": item["arguments"]}})
    return calls


def compact_messages(messages, max_chars, keep_images=2):
    """Keeps the conversation within a size budget.

    Images are the first thing to go (only the newest `keep_images` stay), then the oldest tool results are
    shortened. The system prompt, the task and the most recent exchange are never touched.
    """
    with_images = [m for m in messages if m.get("images")]
    for m in with_images[:-keep_images] if keep_images else with_images:
        m["content"] = (m.get("content") or "") + " [image removed to save context]"
        del m["images"]

    def size():
        return sum(len(m.get("content") or "") + sum(len(i) for i in m.get("images", [])) // 20 for m in messages)

    for m in messages[2:-4]:
        if size() <= max_chars:
            break
        if m["role"] == "tool" and len(m["content"]) > 200:
            m["content"] = m["content"][:150] + " ... [older tool output removed to save context]"
    return messages


def norm_addr(value):
    """Normalise an address argument given by the model.

    "$C000" becomes "0xC000"; a bare string of 3-4 hex digits that contains a letter a-f, or that has 4 digits starting with 0
    ("C000", "0801"), is hexadecimal; other bare digit strings are left alone (decimal).
    """
    if isinstance(value, bool):
        raise ValueError("an address is required")
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if not isinstance(value, str) or not value.strip():
        raise ValueError("an address is required")
    v = value.strip()
    if re.fullmatch(r"\$[0-9A-Fa-f]{1,4}", v):
        return "0x" + v[1:]
    if re.fullmatch(r"[0-9A-Fa-f]{3,4}", v) and (re.search(r"[A-Fa-f]", v) or (len(v) == 4 and v.startswith("0"))):
        return "0x" + v
    return v


def petscii_char(b):
    if 0x41 <= b <= 0x5A or 0x30 <= b <= 0x39 or b in b" !\"#$%&'()*+,-./:;<=>?@[]":
        return chr(b)
    if 0xC1 <= b <= 0xDA:
        return chr(b - 0x80).lower()
    return "."


def petscii_of(hex_text):
    """` "HELLO"` behind a hex dump if the bytes are mostly PETSCII text, else nothing."""
    try:
        data = bytes.fromhex(hex_text)
    except ValueError:
        return ""
    if not data or sum(1 for b in data if petscii_char(b) != "." or b == 0x2E) * 2 < len(data):
        return ""
    return ' "' + "".join(petscii_char(b) for b in data) + '"'


def touched_names(objects, address, length):
    """`  [name, name+2]`: which named variables or data regions a changed range of memory lies in (empty if none is named)."""
    hits = [(start, name) for start, size, name in objects if address < start + size and address + length > start]
    if not hits:
        return ""
    return "  [" + ", ".join(name if address <= start else f"{name}+{address - start}" for start, name in sorted(hits)[:4]) + "]"


def trim(text, limit):
    return text if len(text) <= limit else text[:limit] + f"\n... [{len(text) - limit} more characters cut]"


def to_int(hex_text):
    return int(hex_text, 16)


# --------------------------------------------------------------------------------------------
# tool implementations


class Tools:
    def __init__(self, server):
        self.s = server
        self.finished = False
        self.current = None       # address (hex string) of the routine being processed
        self.range = None         # (first, end) address of that routine, end exclusive
        self.blocks = None        # its own code as [(first, end), ...]; the completeness check looks only at these
        self.finish_warned = False
        self.strict = False       # True: finish_routine accepts a routine only if nothing is missing (no "second call is final")
        self.program = None       # (first, end) of the analysed program
        self.c64 = True           # the C64 machine (I/O, KERNAL, screen); False: generic 64 KiB RAM
        self.vision = True
        self.images = []          # base64 PNGs waiting to be shown to the model

    def reset_routine(self):
        self.finished = False
        self.finish_warned = False
        self.images.clear()

    def dispatch(self, name, args):
        name = clean_tool_name(name)
        if isinstance(args, str):
            try:
                args = json.loads(args) if args.strip() else {}
            except json.JSONDecodeError:
                return "error: arguments must be a JSON object"
        if not isinstance(args, dict):
            return "error: arguments must be an object"
        try:
            clean, notes = prepare_args(name, args)
            fn = getattr(self, "t_" + name, None)
            if fn is None:
                return f"error: unknown tool '{name}'"
            result = fn(**clean)
            return result + ("\n(" + "; ".join(notes) + ")" if notes else "")
        except TypeError as e:
            return f"error: bad arguments for {name}: {e}"
        except (ServerError, ValueError) as e:
            return f"error: {e}"

    # ---- reading ----
    def t_get_listing(self, **a):
        r = self.s.ok(cmd="listing", verify=False, equates=False, addresses=True, max_chars=24000, **{"from": norm_addr(a["from"]), "to": norm_addr(a["to"])})
        return r["listing"] + ("\n[listing truncated]" if r.get("truncated") else "")

    def t_disassemble(self, addr, count=16):
        r = self.s.ok(cmd="dis", addr=norm_addr(addr), count=max(1, min(int(count), 200)))
        return "\n".join(f'{l["addr"]}  {l["bytes"]:<9} {l["text"]}' + (f'   <{l["label"]}>' if "label" in l else "") for l in r["lines"])

    def t_read_memory(self, addr, length=64):
        r = self.s.ok(cmd="read", addr=norm_addr(addr), len=max(1, min(int(length), 1024)))
        data = bytes.fromhex(r["hex"].replace(" ", ""))
        base = to_int(r["addr"])
        rows = []
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            rows.append(f"0x{base + i:04X}: " + " ".join(f"{b:02X}" for b in chunk))
        return "\n".join(rows) + "\npetscii: " + "".join(petscii_char(b) for b in data) + f'\nscreen codes: {r["screen"]}'

    # ---- running ----
    def t_run_routine(self, addr, registers=None, memory=None, max_instructions=200000, screenshot=False, keep_state=False, setup=None,
                      as_interrupt=None):
        s = self.s
        target = to_int(s.ok(cmd="read", addr=norm_addr(addr), len=1)["addr"])
        vector = self.interrupt_vectors().get(target)
        if as_interrupt is None:
            as_interrupt = vector is not None
        # the KERNAL's IRQ entry pushes A, X, Y before it jumps through $0314 (and BRK/NMI likewise); a hardware vector only P and PC
        frame = ("rti" if vector in (0xFFFE, 0xFFFA) else "irq") if as_interrupt else "jsr"
        limit = max(1, min(int(max_instructions), 5_000_000))  # a runaway loop must not stall the whole analysis
        s.ok(cmd="rollback", name="baseline")
        setup_lines = []
        for step in setup or []:
            if not isinstance(step, dict) or "addr" not in step:
                raise ValueError('each setup entry needs an "addr"')
            sregs = {k.upper(): (norm_addr(v) if isinstance(v, str) else v) for k, v in (step.get("registers") or {}).items()}
            sreply = s.call(cmd="call", addr=norm_addr(step["addr"]), regs=sregs, max_instructions=limit)
            if not sreply.get("ok"):
                return f"error: setup routine {step['addr']}: {sreply.get('error')}"
            setup_lines.append(f'setup {step["addr"]}: {"returned" if sreply.get("returned") else "did NOT return (stop: " + sreply["stop"] + ")"}, {sreply["instructions"]} instructions')
        caller_ranges = []
        for m in memory or []:
            if not isinstance(m, dict) or "addr" not in m or ("hex" not in m and "text" not in m):
                raise ValueError('each memory entry needs "addr" and "hex" (or "text")')
            if "hex" in m:
                digits = re.sub(r"0x|[\s,$]", "", str(m["hex"]), flags=re.I)
            else:
                raw = str(m["text"]).encode("latin-1", errors="replace") + (b"" if m.get("nul") is False else b"\0")
                digits = raw.hex()
            s.ok(cmd="write", addr=norm_addr(m["addr"]), hex=digits)
            caller_ranges.append((to_int(s.ok(cmd="read", addr=norm_addr(m["addr"]), len=1)["addr"]), len(digits) // 2))
        regs = {k.upper(): (norm_addr(v) if isinstance(v, str) else v) for k, v in (registers or {}).items()}
        s.ok(cmd="checkpoint", name="pre_run")  # what the routine changes is measured from here: the memory the caller set up does not count
        screen_before = None
        if self.c64:
            s.ok(cmd="io")           # empty the chip-write log
            s.ok(cmd="kernalcalls")  # ... the KERNAL-call log
            s.ok(cmd="console")      # ... and the console
            screen_before = s.ok(cmd="screen")["lines"]
            s.ok(cmd="screenshot", name="_before")
        reply = s.call(cmd="call", addr=norm_addr(addr), regs=regs, max_instructions=limit, frame=frame)
        if not reply.get("ok"):
            s.call(cmd="drop", name="pre_run")
            return f"error: {reply.get('error')}"
        r = reply["regs"]
        out = setup_lines + ([f"run as an interrupt handler (frame: {'PC, P, A, X, Y as the KERNAL pushes them' if frame == 'irq' else 'PC and P'}; it returns with RTI)"]
                             if frame != "jsr" else [])
        out.append(f'stop: {reply["stop"]}  returned: {reply.get("returned")}  instructions: {reply["instructions"]}  cycles: {reply["cycles"]}')
        out.append(f'registers after: A={r["A"][2:]} X={r["X"][2:]} Y={r["Y"][2:]} S={r["S"][2:]} P={r["P"][2:]} flags={r["flags"]}')
        if reply["stop"] == "halted":
            out.append(f'NOTE: the processor ran into a JAM opcode at {reply["pc"]}: execution went somewhere that is not code (a wrong address, '
                       "or a jump through a pointer/vector that is not set up).")
        elif reply["stop"] == "exit":
            out.append(f'NOTE: the KERNAL emulation ended the run: {reply.get("message")} (at {reply["pc"]}).')
        elif reply["stop"] != "returned" and not reply.get("note"):
            out.append(f'NOTE: it did not return (stop: {reply["stop"]}). For a program entry or a main loop that is normal: the state shown is the '
                       f'state after {reply["instructions"]} instructions. Use a smaller max_instructions to look at an earlier moment.')
        if reply.get("note"):
            out.append("NOTE: " + reply["note"])
        if reply.get("interrupts"):
            out.append(f'interrupts taken during the run: {reply["interrupts"]} (BRK counts as one)')

        diff = s.ok(cmd="diff", name="pre_run")
        s.call(cmd="drop", name="pre_run")
        shown = [x for x in diff["ranges"] if not (0x0100 <= to_int(x["addr"]) < 0x0200) and x.get("area") != "color"]
        color = [x for x in diff["ranges"] if x.get("area") == "color"]
        if shown:
            out.append(f"memory changed ({len(shown)} ranges; the stack page is omitted; small ranges first):")
            objects = self.named_objects()
            for x in sorted(shown, key=lambda x: (x["len"], to_int(x["addr"])))[:14]:
                before, after = x["before"].replace(" ", ""), x["after"].replace(" ", "")
                out.append(f'  {x["addr"]} len {x["len"]}: {before[:32]} -> {after[:32]}' + petscii_of(after[:32]) + touched_names(objects, to_int(x["addr"]), x["len"]))
            if len(shown) > 14:
                out.append(f"  ... and {len(shown) - 14} larger ranges (use read_memory to look at them)")
        else:
            out.append("memory changed: nothing (outside the stack)")
        if color:
            out.append(f"colour RAM changed: {sum(x['len'] for x in color)} bytes ({color[0]['addr']} ...)")
        out += self.write_notes(shown, caller_ranges)

        if self.c64:
            writes = s.ok(cmd="io")["output"]
            if writes:
                lo, hi = (to_int(self.range[0]), to_int(self.range[1])) if self.range else (0, 0)
                own = [w for w in writes if lo <= to_int(w["pc"]) < hi]
                other = [w for w in writes if not lo <= to_int(w["pc"]) < hi]
                out.append(f"writes to chip registers: {len(writes)} in total - {len(own)} by this routine's own instructions, "
                           f"{len(other)} by the code it calls (do NOT attribute those to this routine's lines):")
                for label, group, cap in (("this routine", own, 10), ("called code", other, 5)):
                    for w in group[:cap]:
                        out.append(f'  {w["addr"]} <- {w["value"]} ({w["chip"]}{" " + w["what"] if "what" in w else ""}) by the instruction at {w["pc"]} [{label}]')
                    if len(group) > cap:
                        out.append(f"  ... and {len(group) - cap} more from {label}")
            calls = s.ok(cmd="kernalcalls").get("calls", [])
            if calls:
                out.append(f"KERNAL calls made: {len(calls)}")
                for c in calls[:12]:
                    a, o = c["regs_in"], c["regs_out"]
                    out.append(f'  {c["name"] or "unknown entry " + c["addr"]}' + ("" if c["emulated"] else " [NOT emulated: only logged, returned with C = 0]") +
                               f' A={a["A"][2:]} X={a["X"][2:]} Y={a["Y"][2:]} -> A={o["A"][2:]} X={o["X"][2:]} Y={o["Y"][2:]} flags={o["flags"]}' +
                               (f' [{c["note"].strip()}]' if c.get("note") else "") + f' (the JSR is at {c["call_site"]})')
                if len(calls) > 12:
                    out.append(f"  ... and {len(calls) - 12} more")
                if any(not c["emulated"] for c in calls):
                    out.append("NOTE: KERNAL calls that are not emulated return at once; the routine may then take an error path. That is a limit of the "
                               "emulator; describe what the call is MEANT to do from its name and registers.")
            text = s.ok(cmd="console").get("text", "")
            if text:
                out.append(f"text printed with CHROUT: {text!r}")
            after = s.ok(cmd="screen")["lines"]
            changed = [(i, line) for i, (old, line) in enumerate(zip(screen_before, after)) if old != line]
            d = s.ok(cmd="screen_diff", a="_before")
            if not changed and d.get("changed_pixels", 0) == 0:
                out.append("screen: unchanged")
            else:
                if d.get("changed_pixels"):
                    c = d["text_cells"]
                    out.append(f'screen: {d["changed_pixels"]} pixels changed, columns {c["col0"]}-{c["col1"]}, rows {c["row0"]}-{c["row1"]} of 40x25')
                for i, line in changed[:8]:
                    out.append(f"  row {i:2}: {line!r}")
                if len(changed) > 8:
                    out.append(f"  ... and {len(changed) - 8} more rows")
            if screenshot:
                if d.get("changed_pixels", 0) > 0:
                    c = d["text_cells"]
                    crop = {"col0": max(c["col0"] - 2, 0), "row0": max(c["row0"] - 1, 0), "col1": min(c["col1"] + 2, 39), "row1": min(c["row1"] + 1, 24)}
                    shot = s.ok(cmd="screenshot", name="_after", crop=crop)
                    out.append(f"(image attached: columns {crop['col0']}-{crop['col1']}, rows {crop['row0']}-{crop['row1']}, zoom {shot['zoom']}x)")
                else:
                    shot = s.ok(cmd="screenshot", name="_after")
                    out.append("(image attached: full screen)")
                if self.vision:
                    self.images.append(shot["png_base64"])
                else:
                    out[-1] += "  [NOT shown: you cannot see images; use the text summary above]"
        if not keep_state:
            s.ok(cmd="rollback", name="baseline")
        else:
            out.append("state kept; call reset_machine to restore.")
        return "\n".join(out)

    def interrupt_vectors(self):
        """{handler address: vector} of the interrupt handlers the analysis found."""
        if not hasattr(self, "_vectors"):
            self._vectors = {to_int(r["addr"]): to_int(r["interrupt_vector"]) for r in self.s.ok(cmd="routines")["routines"] if r.get("interrupt_vector")}
        return self._vectors

    def t_screenshot(self, col0=None, row0=None, col1=None, row1=None, zoom=None):
        if not self.c64:
            return "error: there is no screen on the generic machine"
        req = {"cmd": "screenshot", "name": "_view"}
        if None not in (col0, row0, col1, row1):
            req["crop"] = {"col0": col0, "row0": row0, "col1": col1, "row1": row1}
        if zoom:
            req["zoom"] = int(zoom)
        shot = self.s.ok(**req)
        text = "\n".join(f"  {line}" for line in self.s.ok(cmd="screen")["lines"] if line)
        if not self.vision:
            return "(you cannot see images: this model has no vision) The screen as text:\n" + text
        self.images.append(shot["png_base64"])
        return f'(image attached: {shot["width"]}x{shot["height"]} px, screen mode {shot["mode"]}) The screen as text:\n' + text

    def named_objects(self):
        """(address, size, name) of the variables and named data regions annotated so far."""
        ann_list = self.s.ok(cmd="ann_list")["annotations"]
        objects = [(to_int(v["addr"]), max(1, v.get("size", 1)), v["name"]) for v in ann_list.get("variables", []) if v.get("name")]
        objects += [(to_int(d["addr"]), d["length"], d["name"]) for d in ann_list.get("data", []) if d.get("name")]
        return objects

    def write_notes(self, shown, caller_ranges=()):
        """Writes into the code of a routine (self-modifying code, or an overrun) or past the end of the program."""
        notes = []
        if not shown:
            return notes
        routines = None
        for x in shown:
            a, n = to_int(x["addr"]), x["len"]
            if any(a >= ca and a + n <= ca + cl for ca, cl in caller_ranges):
                continue                     # inside memory the caller set up for the test: that is what it was for
            if a < 0x0400 or 0xD000 <= a < 0xE000:
                continue                     # zero page, KERNAL work area, chips: normal
            if routines is None:
                routines = self.s.ok(cmd="routines")["routines"]
            hit = next(((r, bs, be) for r in routines for bs, be in Annotator.blocks(r) if a < to_int(be) and a + n > to_int(bs)), None)
            if hit:
                r, bs, be = hit
                notes.append(f'NOTE: the routine changed memory inside the code of {r["name"]} (0x{to_int(bs):04X}-0x{to_int(be) - 1:04X}) at 0x{a:04X}: '
                             "self-modifying code (common on the 6510), or an overrun into the code?")
        return notes[:4]

    def t_remove_annotation(self, addr):
        r = self.s.ok(cmd="ann_remove", addr=norm_addr(addr))
        return "ok: removed" if r.get("removed") else "nothing was annotated at that address"

    def t_reset_machine(self):
        self.s.ok(cmd="rollback", name="baseline")
        return "machine restored"

    # ---- annotating ----
    def t_name_label(self, addr, name):
        self.s.ok(cmd="ann_label", addr=norm_addr(addr), name=name)
        return f"ok: {name}"

    def t_document_routine(self, addr, name, summary, inputs="", outputs="", clobbers="", confidence=""):
        self.s.ok(cmd="ann_routine", addr=norm_addr(addr), name=name, summary=summary, inputs=inputs, outputs=outputs,
                  clobbers=clobbers, confidence=confidence)
        return f"ok: routine {name} documented"

    def t_name_variable(self, addr, name, size=1, comment=""):
        self.s.ok(cmd="ann_variable", addr=norm_addr(addr), name=name, size=int(size), comment=comment)
        return f"ok: variable {name}"

    def t_mark_data(self, addr, kind, length=None, name="", comment="", partner=None, minus_one=False):
        if length is None:
            if kind != "text":
                raise ValueError("length is required unless kind is text (then the string ends with its zero byte)")
            raw = self.s.ok(cmd="read", addr=norm_addr(addr), len=4096)["hex"].replace(" ", "")
            nul = next((i // 2 for i in range(0, len(raw), 2) if raw[i:i + 2] == "00"), None)
            if nul is None:
                raise ValueError("no terminating zero byte within 4096 bytes: give the length")
            length = nul + 1
        req = {"cmd": "ann_data", "addr": norm_addr(addr), "length": int(length), "kind": kind, "name": name, "comment": comment}
        if kind == "lohi":
            if partner is None:
                raise ValueError("kind lohi needs `partner`: the address of the table of high bytes")
            req["partner"] = norm_addr(partner)
            req["minus_one"] = bool(minus_one)
        self.s.ok(**req)
        return "ok: data region marked" + (" (tables of code addresses are followed after the next analysis)" if kind in ("pointers", "rts_pointers", "lohi") else "")

    def t_add_comment(self, addr, text, block=False):
        self.s.ok(cmd="ann_comment", addr=norm_addr(addr), text=text, block=bool(block))
        return "ok"

    def t_mark_constant(self, addr, remove=False):
        self.s.ok(cmd="ann_constant", addr=norm_addr(addr), remove=bool(remove))
        return "ok: constant mark removed" if remove else "ok: constant marked"

    def completeness_problems(self, rng=None):
        """What is still missing in a routine's listing (names, comments); `rng` is one (first, end) or a list of them,
        default: the routine being processed (its blocks)."""
        if rng is None:
            rng = self.blocks or ([self.range] if self.range else [])
        elif rng and isinstance(rng[0], str):
            rng = [rng]
        if not rng:
            return []
        r = {"instructions": 0}
        texts = []
        for first, end in rng:
            part = self.s.ok(cmd="listing", verify=False, max_chars=200000, **{"from": first, "to": end})
            texts.append(part["listing"])
            r["instructions"] += part.get("instructions", 0)
            if "user_comments" in part:
                r["user_comments"] = r.get("user_comments", 0) + part["user_comments"]
        text = "\n".join(texts)
        problems = []
        unnamed = re.findall(r"^((?:loc|sub|data)_[0-9A-Fa-f]{4}):", text, re.M)
        if unnamed:
            problems.append(f"labels without a real name: {', '.join(unnamed[:8])}{' ...' if len(unnamed) > 8 else ''} (name_label)")
        numeric = set()
        for line in text.splitlines():
            m = re.match(r"\s+([A-Z]{3}(?:\.W)?)\s+([^;]*)", line)
            if not m or "#" in m.group(2) or m.group(1) in ("JMP", "JSR") and "(" not in m.group(2):
                continue
            for h in re.findall(r"\$([0-9A-Fa-f]{2,4})\b", m.group(2)):
                numeric.add(int(h, 16))
        # not the stack page, not the chips and ROMs, not the screen; zero page from $02 (the processor port is $00/$01)
        numeric = sorted(a for a in numeric if 2 <= a < 0x100 or 0x0200 <= a < 0x0400 or 0x0800 <= a < 0xA000 or 0xC000 <= a < 0xD000)
        if numeric:
            problems.append("memory locations used by number without a name: " + ", ".join(f"${a:04X}" if a > 0xFF else f"${a:02X}" for a in numeric[:8]) +
                            (" ..." if len(numeric) > 8 else "") + " (name_variable, or mark_data for tables)")
        problems += self.documentation_problems()
        wanted = min(max(2, r["instructions"] // 10), max(1, r["instructions"] // 2))   # about one per 10 instructions, but never more than one per two
        if r.get("user_comments", wanted) < wanted:
            problems.append(f"only {r['user_comments']} comment(s) for {r['instructions']} instructions - add about {wanted} short ones with add_comment "
                            "(explain meaning and purpose of the non-obvious steps)")
        return problems

    def documentation_problems(self):
        """document_routine accepts a bare name and summary; the routine's contract is what the next reader needs."""
        if self.current is None:
            return []
        doc = next((r for r in self.s.ok(cmd="routines")["routines"] if to_int(r["addr"]) == to_int(self.current)), None)
        if not doc or not doc["documented"]:
            return []
        problems = []
        missing = [k for k in ("inputs", "outputs", "clobbers") if not (doc.get(k) or "").strip()]
        if missing:
            problems.append(f"document_routine without {', '.join(missing)} (say \"none\" if there is nothing to say)")
        if not doc.get("confidence"):
            problems.append("document_routine without `confidence` (high = confirmed by a test you ran, medium = plausible from the code, low = a guess)")
        return problems

    def t_finish_routine(self, summary=""):
        still_open = {to_int(r["addr"]) for r in self.s.ok(cmd="routines", undocumented_only=True)["routines"]}
        if self.current is not None and to_int(self.current) in still_open:
            return "error: call document_routine for this routine first (name, summary, inputs, outputs, clobbers)."
        if self.strict or not self.finish_warned:
            self.finish_warned = True
            problems = self.completeness_problems()
            if problems:
                again = ("call finish_routine again once it is fixed" if self.strict
                         else "then call finish_routine again (a second call is accepted as final)")
                return ("not finished yet - the listing of this routine is still incomplete:\n- " + "\n- ".join(problems) +
                        f"\nFix this with the annotation tools, {again}.")
        self.finished = True
        return "ok: routine finished"


# --------------------------------------------------------------------------------------------
# the annotation workflow

SYSTEM_PROMPT = """You are an expert 6510 (6502) and Commodore 64 reverse engineer. You document ONE subroutine at a time so that the final
assembler listing reads like the original source code, with meaningful names and helpful comments.

How you work:
0. The listing uses the usual 6502 syntax: $ = hexadecimal, #$10 = immediate, #<label / #>label = low / high byte of an address,
   LDA.W forces the absolute form of a zero-page address. The right-hand comment of every line starts with the line's ADDRESS; copy
   addresses from there and never count bytes yourself. Names and comments only work at those exact addresses.
1. Read the listing of the routine. KERNAL calls (CHROUT, GETIN, ...) and chip registers (EXTCOL = $D020, ...) appear by name. Routines
   called by this one that are already documented appear with their real names. Placeholders (sub_XXXX, loc_XXXX, data_XXXX) await a real name.
2. Form a hypothesis, then TEST it: use run_routine with suitable registers/memory, and ask for a screenshot when the routine draws on the
   screen. Compare the registers, memory changes, chip writes, KERNAL calls and screen changes with what you expected. Do not guess when you can run it.
3. Record what you learned with the annotation tools: document_routine (name, summary, inputs, outputs, clobbers), name_variable for
   every memory location that has a meaning (zero-page pointers with size 2), mark_data for tables/strings, name_label for loops and branch
   targets, add_comment for the non-obvious lines. An LDA #<label / LDY #>label pair whose value is a number and not an address:
   mark_constant on both instructions.
4. Call finish_routine when done.

6510 / C64 facts that are easy to get wrong:
- A, X, Y are 8 bit; S is the stack pointer into page 1 ($0100-$01FF), the stack grows DOWNWARDS. P holds the flags N V - B D I Z C.
  JSR pushes the return address MINUS ONE, RTS adds one. There are no 16 bit registers: 16 bit values live in two bytes (low byte first).
- ADC adds the carry: CLC before an addition; SBC subtracts the inverted carry: SEC before a subtraction. CMP sets C when A >= operand.
  With the D flag set ADC/SBC work in BCD. INX/DEX/INY/DEY and INC/DEC do not touch the carry.
- (zp),Y reads a 16 bit pointer from two zero-page bytes and adds Y; (zp,X) adds X to the zero-page address first. zp,X wraps within page 0.
- A jump table may push the address MINUS ONE of a routine and RTS to it ("RTS trick"), or load a vector and JMP (vector). The analysis
  shows such tables as .LO/.HI or .WORD lines; their entries are handlers reached through the table.
- Self-modifying code is common: an instruction stores into the operand of another one (STA loop+1). The listing shows label+1.
- Memory: $0400-$07E7 screen (screen codes: @=0, A=1 ... Z=26, space=32), $D800 colour RAM, $D000 VIC-II, $D400 SID, $DC00 CIA 1
  (keyboard, joystick, timers), $DD00 CIA 2 (VIC bank), $A000 BASIC ROM, $E000 KERNAL ROM. $01 switches ROMs and I/O in and out
  ($37 normal, $35 all RAM except I/O, $34 all RAM). $D020 border colour, $D021 background colour, $D019/$D01A raster interrupt.
- Text for CHROUT is PETSCII (upper case letters $41-$5A, RETURN = $0D, clear screen = $93). Zero page $FB-$FE is free for programs;
  most other zero-page bytes belong to BASIC and the KERNAL.
- KERNAL: CHROUT ($FFD2) prints A, GETIN ($FFE4) reads a key into A (0 = none), CHRIN ($FFCF) reads a line, PLOT ($FFF0) cursor X = row,
  Y = column, SETLFS/SETNAM/LOAD/SAVE files. The emulator really emulates the common calls; others are only LOGGED by run_routine and
  return at once ("NOT emulated"). An interrupt handler installed through $0314/$0315 usually ends with JMP $EA31 (the KERNAL's own IRQ code).

The listing may contain jump tables (.LO/.HI or .WORD lines with sub_ names): describe what each case does and name the case labels
(name_label). Strings and screen texts the analysis recognised are already shown as .BYTE "..." or .SCREEN "..." lines: give them a
meaningful name with mark_data (same address, same kind, a name) and say what they are used for.

Do not finish early. Before finish_routine: give EVERY label a name (loops, branch targets still called loc_/sub_/data_), name every
memory location the code uses (name_variable, with a comment on its meaning), mark tables and strings (mark_data) and add a short comment
(add_comment) to every non-obvious step. finish_routine checks this once and tells you what is missing.

JSON: write every tool argument as valid JSON. Addresses and hex values are STRINGS in quotes ("0xC000"), never bare 0xC000 (that makes
the call fail).

Efficiency: you may and should make SEVERAL tool calls in ONE reply (for example all name_variable and add_comment calls together, then
finish_routine). Every reply costs time, and you only have a limited number of replies per routine.

A routine often depends on state that other code sets up: variables, pointers, the screen. Run it with `setup` (the routine that initialises
things) or write the expected values with `memory` instead of guessing.

Be brief. Do not re-derive 6502 basics and do not repeat yourself. Your FIRST reply must already be a tool call (usually run_routine).
Prefer testing over long reasoning: a run tells you more than a page of thinking.

Naming: snake_case ASCII, unique, describing purpose not address. Routines start with a verb (print_string, read_joystick), variables
are nouns (player_x, lives_left), flags end in a state (sound_enabled). Never reuse register names (A, X, Y), mnemonics, KERNAL names or
chip register names.
Comments explain WHY and meaning, not what the instruction obviously does; keep them short. Mention units (pixels, characters, frames)
and valid ranges when you know them. If you are not sure, say so in the comment ("probably ...") instead of inventing details.
Set `confidence` in document_routine honestly: high only if a test you ran confirms it, medium if it is plausible from the code, low if it
is a guess. Another model may check your low/medium routines later - that is fine, a clear "low" is more useful than a confident mistake.
Write all summaries and comments in {lang}.
"""

REFINE_PROMPT = """
THIS IS A REFINEMENT PASS. A faster model has already annotated this routine; its names, comments and documentation appear in the listing.
Treat them as hypotheses of a colleague, not as facts: verify the important claims by running the routine, correct what is wrong (calling
document_routine, name_label, name_variable or add_comment again for the same address overwrites the old text) and close the gaps listed in
the task. Do not rewrite things that are already right. Set `confidence` to high only for what you verified.
"""

NO_VISION = "\nYou cannot see images: screenshots are not shown to you; use the text of the screen that run_routine and screenshot report.\n"


class Annotator:
    """Runs one or more passes over the program. Pass 1 explores; later passes refine routines that still have gaps."""

    def __init__(self, server, passes, args, log=None):
        self.s, self.passes, self.a, self.log = server, passes, args, log
        self.llm = passes[0]["llm"] if passes else None   # no model in a dry run
        self.max_turns = args.max_turns
        self.entries = []
        self.mode = "explore"
        self.tools = Tools(server)

    said_thinking = False
    chat_ok = False        # becomes True with the first successful answer: errors before that are configuration problems
    model_errors = 0       # consecutive routines that failed because of the model server

    def say(self, text):
        print(text, flush=True)

    def log_event(self, **ev):
        if self.log:
            self.log.write(json.dumps(ev, default=str) + "\n")
            self.log.flush()

    # ---- setup ----
    def analyze(self):
        req = {"cmd": "analyze", "reset": True}
        if self.entries:
            req["entries"] = self.entries
        return self.s.ok(**req)

    def setup(self):
        a, s = self.a, self.s
        check_server(s)
        s.ok(cmd="machine", type=a.machine)
        self.tools.c64 = a.machine == "c64"
        if a.prg:
            r = s.ok(cmd="load_file", file=a.prg, **({"addr": a.org} if a.org else {}))
            self.say(f'program loaded: {r["start"]}-{r["end"]}, entry {r["entry"]}' + (f' ({r["basic"]})' if r.get("basic") else ""))
        else:
            if not a.org:
                raise ServerError("--bin needs --org (the load address)")
            r = s.ok(cmd="load_file", file=a.bin, type="raw", addr=a.org)
            self.say(f'binary loaded: {r["start"]}-{r["end"]}')
        if a.project and os.path.exists(a.project):
            s.ok(cmd="ann_load", file=a.project)
            self.say("project loaded")
        self.entries = list(a.entry or [])
        r = self.analyze()
        self.tools.program = (to_int(r["from"]), to_int(r["to"]))   # [first, end): `to` is the end of the analysed range
        self.say(f'analysed {r["instructions"]} instructions, {r["from"]}-{r["to"]}, {r["routines"]} routines'
                 + (f', {r["jump_tables"]} jump tables' if r.get("jump_tables") else "")
                 + (f', {len(r["interrupt_handlers"])} interrupt handler(s)' if r.get("interrupt_handlers") else ""))
        if not a.no_auto_tables:
            self.follow_pointer_tables()
        if not a.no_auto_data:
            self.mark_data_regions()
        s.ok(cmd="checkpoint", name="baseline")

    def mark_data_regions(self):
        """Strings, screen texts, sprites, character sets and word tables that the analysis recognises are marked as data regions, so the
        model sees them in their form in the listing and no routine analysis can mistake them for code. Zero runs and the BASIC starter need no
        annotation (the listing writes .FILL and .BASIC)."""
        r = self.s.ok(cmd="data_regions", apply=True)
        if r.get("applied"):
            self.analyze()
            kinds = Counter(x["kind"] for x in r["regions"] if x.get("applied"))
            self.say(f'{r["applied"]} data regions marked: ' + ", ".join(f"{n} {k}" for k, n in sorted(kinds.items())))

    def follow_pointer_tables(self):
        """Tables of code addresses that the analysis only suggests are marked as pointer tables and followed, so that their handlers are
        analysed as routines. Marking only adds a name and a display form; a false positive costs nothing but a few extra routines."""
        s = self.s
        for _ in range(3):  # following one table can reveal another
            tables = [t for t in s.ok(cmd="jump_tables", max_entries=1)["tables"] if not t["followed"]]
            marked = 0
            for t in tables:
                name = f'jump_table_{int(t["table"], 16):04x}'
                if s.call(cmd="ann_data", addr=t["table"], length=t["bytes"], kind="pointers", name=name).get("ok"):
                    marked += 1
            if not marked:
                break
            r = self.analyze()
            self.say(f'{marked} pointer table(s) followed: now {r["routines"]} routines')

    # ---- one routine ----
    @staticmethod
    def extent(r):
        """(first address, end address exclusive) of a routine as hex strings: the hull of its code."""
        return r["start"], r["end"]

    @staticmethod
    def blocks(r):
        """The routine's own code as [(start, end), ...] (hex strings, end exclusive)."""
        found = [(b[0], b[1]) for b in (r.get("blocks") or [])]
        return found or [(r["start"], r["end"])]

    @staticmethod
    def own_bytes(r):
        return sum(to_int(e) - to_int(s) for s, e in Annotator.blocks(r))

    def routine_prompt(self, r, gaps=None):
        addr = r["addr"]
        blocks = self.blocks(r)
        parts, refs, truncated = [], [], False
        seen_refs = set()
        for i, (bs, be) in enumerate(blocks, 1):
            lst = self.s.ok(cmd="listing", verify=False, equates=False, addresses=True, max_chars=max(3000, 14000 // len(blocks)), **{"from": bs, "to": be})
            truncated = truncated or bool(lst.get("truncated"))
            head = f"; ---- part {i} of {len(blocks)}: {bs}-{be} (other code lies in between; it is not part of this routine) ----\n" if len(blocks) > 1 else ""
            parts.append(head + lst["listing"])
            for x in self.s.ok(cmd="references", **{"from": bs, "to": be})["references"]:
                if x["addr"] not in seen_refs:
                    seen_refs.add(x["addr"])
                    refs.append(x)
        callees = [f'  {c["name"]} ({c["addr"]}{"" if c["documented"] else ", not documented yet"})' for c in r["callees"]]
        kernal = {k["addr"]: k["name"] for k in r.get("kernal_calls", [])}
        callees += [f"  {kernal[x]} ({x}, KERNAL)" if x in kernal else f"  {x} (outside the analysed code)" for x in r["external_calls"]]
        if r.get("callers"):
            callers = ", ".join(r["callers"])
        elif r.get("interrupt_vector"):
            callers = f'nobody: it is an interrupt handler installed through the vector at {r["interrupt_vector"]}'
        elif r.get("via_table"):
            callers = "reached through a jump table (no direct call)"
        else:
            callers = "none (entry point)"
        size = f'{self.own_bytes(r)} bytes' + (f' in {len(blocks)} parts' if len(blocks) > 1 else "")
        io = r.get("io_access", {})
        facts = []
        if io.get("reads") or io.get("writes"):
            facts.append("chip registers: " + ", ".join([f'reads {x["name"] or x["addr"]}' for x in io.get("reads", [])] +
                                                        [f'writes {x["name"] or x["addr"]}' for x in io.get("writes", [])]))
        if r.get("zero_page"):
            facts.append("zero page used: " + ", ".join(r["zero_page"]))
        if r.get("port_writes"):
            facts.append("writes the processor port $01 (memory configuration): " +
                         ", ".join(f'at {p["at"]}' + (f' value {p["value"]}' if "value" in p else "") for p in r["port_writes"]))
        text = (f'ROUTINE_ADDR={addr}  current name: {r["name"]}  size: {size}  instructions: {r["instructions"]}\n'
                f'Called from: {callers}\nCalls:\n' + ("\n".join(callees) if callees else "  nothing") + "\n" +
                ("".join(f"{f}\n" for f in facts)) +
                f"\nListing:\n```\n" + "\n".join(parts) + f"\n```\n{'[listing truncated; use get_listing for the rest]' if truncated else ''}\n"
                + self.references_text(refs) +
                "Analyse this routine, test it, annotate it and call finish_routine.")
        if gaps:
            text += ("\n\nKnown gaps to close in this pass:\n- " + "\n- ".join(gaps) +
                     "\nVerify the existing documentation as well, and lower or raise its confidence accordingly.")
        return text

    @staticmethod
    def references_text(refs):
        """What the operands of the listing point at (strings, tables, variables, other routines, chip registers)."""
        if not refs:
            return ""
        lines = ["Referenced from this routine:"]
        for x in refs[:40]:
            what = x["kind"] + (f", {x['length']} bytes" if x.get("length") else "")
            body = ""
            if x.get("preview"):
                body = ": " + (f'"{x["preview"]}"' if x["kind"] in ("text", "screen") else x["preview"])
            lines.append(f'  {x["addr"]} {x["name"]} ({what}){body}')
        if len(refs) > 40:
            lines.append(f"  ... and {len(refs) - 40} more")
        return "\n".join(lines) + "\n\n"

    def process_routine(self, r, gaps=None):
        t = self.tools
        t.reset_routine()
        t.current = r["addr"]
        t.range = self.extent(r)
        t.blocks = self.blocks(r)
        t.vision = getattr(self.llm, "vision", True)
        system = SYSTEM_PROMPT.format(lang=self.a.lang)
        if self.mode == "refine":
            system += REFINE_PROMPT
        if not t.vision:
            system += NO_VISION
        messages = [{"role": "system", "content": system},
                    {"role": "user", "content": self.routine_prompt(r, gaps)}]
        nudges = 0
        cut = 0
        bad_json = 0
        repeats = {}
        try:
            for turn in range(self.max_turns):
                if self.max_turns >= 6 and turn == self.max_turns - 3:
                    messages.append({"role": "user", "content": "Only 3 rounds are left for this routine. In your next reply call ALL remaining annotation tools "
                                                                "together (several tool calls in one reply are fine) and finish_routine."})
                compact_messages(messages, max_chars=self.a.num_ctx * 3)
                try:
                    resp = self.llm.chat(messages, TOOLS)
                except OllamaFormatError as e:
                    bad_json += 1
                    self.say(f"  the model wrote an invalid tool call ({bad_json}x): {str(e)[:110]}")
                    self.log_event(routine=r["addr"], turn=turn, invalid_tool_call=str(e)[:300])
                    if bad_json > 4:
                        return False
                    messages.append({"role": "user", "content": "Your last tool call could not be parsed because it was not valid JSON. Write ALL arguments as valid JSON: "
                                                                'addresses and hex values MUST be strings in quotes, e.g. "0xC000" - never a bare 0xC000. Try again.'})
                    continue
                self.chat_ok = True
                msg = resp.get("message", {})
                if msg.get("thinking") and not self.llm.think and not self.said_thinking:
                    self.said_thinking = True
                    self.say("  note: this model thinks even with --think false (a thinking-only model tag). It works, but slowly; "
                             "an -instruct tag is faster. The answer limit grows automatically when it is cut off.")
                self.log_event(routine=r["addr"], turn=turn, message=msg)
                echoed = {k: v for k, v in msg.items() if k in ("content", "tool_calls")}
                echoed["role"] = "assistant"
                messages.append(echoed)
                calls = msg.get("tool_calls") or calls_from_content(msg.get("content"))
                if not calls and resp.get("done_reason") == "length":
                    # the answer hit the token limit (usually: endless thinking) - nothing usable was produced
                    cut += 1
                    messages.pop()  # the empty, cut-off answer carries no information: do not keep it in the context
                    old = self.llm.options["num_predict"]
                    new = min(int(old * 1.5), self.llm.max_tokens_limit)
                    self.llm.options["num_predict"] = new  # a model that needs long thinking gets room for it
                    self.say(f'  answer cut off at the token limit ({cut}x, {old} tokens); '
                             + (f"raising the limit to {new}" if new > old else "limit already at the maximum") +
                             '. Alternatives: a smaller thinking effort, or an "-instruct" model tag')
                    if cut > 5 or (new == old and cut > 2):
                        return False
                    messages.append({"role": "user", "content": "Your last answer was cut off because it was too long. Do NOT deliberate: pick ONE concrete next step "
                                                                "(for example run_routine, or document_routine) and call the tool right now."})
                    continue
                if not calls:
                    nudges += 1
                    if nudges > 2:
                        self.say(f'  model stopped without finishing {r["addr"]}')
                        return False
                    messages.append({"role": "user", "content": "Continue with tool calls. When the routine is documented, call finish_routine."})
                    continue
                for call in calls:
                    fn = call.get("function", {})
                    name, args = clean_tool_name(fn.get("name", "")), fn.get("arguments", {})
                    result = t.dispatch(name, args)
                    key = (name, json.dumps(args, sort_keys=True, default=str))
                    if result.startswith("error"):
                        repeats[key] = repeats.get(key, 0) + 1
                        if repeats[key] >= 3:
                            result += "\nYou have made this same failing call several times. Change your approach."
                    self.log_event(routine=r["addr"], tool=name, args=args, result=trim(result, 2000))
                    messages.append({"role": "tool", "tool_name": name, "content": trim(result, 6000)})
                if t.images:
                    messages.append({"role": "user", "content": "Screenshot(s) from the last tool call:", "images": list(t.images)})
                    t.images.clear()
                if t.finished:
                    return True
            self.say(f'  turn limit reached for {r["addr"]}')
            still_open = {to_int(x["addr"]) for x in self.s.ok(cmd="routines", undocumented_only=True)["routines"]}
            if to_int(r["addr"]) not in still_open:
                self.say("  (the routine is documented, only finish_routine was missing: counted as partial; a refinement pass will check it)")
                return "partial"
            return False
        finally:
            t.current = None

    def process_routine_safely(self, r, gaps=None):
        """A server error on one routine must not end a run that may have taken hours: note it, go on with the next one.

        Errors before the first successful answer (wrong host, unknown model) are configuration problems and abort at once;
        three routines in a row failing means the server is really gone."""
        try:
            result = self.process_routine(r, gaps)
            self.model_errors = 0
            return result
        except OllamaError as e:
            if not self.chat_ok:
                raise
            self.model_errors += 1
            self.say(f'  model error on {r["addr"]}: {str(e)[:160]} - skipping this routine ({self.model_errors} in a row)')
            self.log_event(routine=r["addr"], model_error=str(e)[:300])
            if self.model_errors >= 3:
                raise
            return False

    # ---- gaps ----
    def routine_gaps(self, r):
        """Objective list of what is missing or doubtful about a routine's documentation."""
        gaps = [] if r["documented"] else ["the routine is not documented yet (document_routine)"]
        gaps += self.tools.completeness_problems(self.blocks(r))
        conf = r.get("confidence", "")
        if conf == "low" or (conf == "medium" and self.a.refine_medium):
            gaps.append(f"the previous model marked its own documentation as confidence {conf}: verify it by running the routine")
        return gaps

    def only_filter(self, routines):
        if self.a.only:
            wanted = {int(str(norm_addr(x)), 0) if str(norm_addr(x)).lower().startswith("0x") else int(x) for x in self.a.only}
            routines = [r for r in routines if to_int(r["addr"]) in wanted]
        return routines

    # ---- passes ----
    def explore_pass(self):
        s, done, tried = self.s, Counter(), set()
        for _ in range(3):  # documenting a routine can reveal that others are still open
            routines = self.only_filter(s.ok(cmd="routines", undocumented_only=True)["routines"])
            routines = [r for r in routines if r["addr"] not in tried]
            if not routines:
                break
            tried |= {r["addr"] for r in routines}
            for i, r in enumerate(routines, 1):
                self.say(f'[{i}/{len(routines)}] {r["name"]} at {r["addr"]} ({self.own_bytes(r)} bytes)')
                done[self.process_routine_safely(r)] += 1
                self.save()
        return done

    def refine_pass(self):
        s, done = self.s, Counter()
        routines = self.only_filter(s.ok(cmd="routines")["routines"])
        todo = []
        for r in routines:
            gaps = self.routine_gaps(r)
            if gaps or (self.a.refine_all and r["documented"]):
                todo.append((r, gaps))
        self.say(f"{len(todo)} of {len(routines)} routines have gaps")
        for i, (r, gaps) in enumerate(todo, 1):
            self.say(f'[{i}/{len(todo)}] {r["name"]} at {r["addr"]}: ' + "; ".join(g.split(" (")[0] for g in gaps)[:140])
            done[self.process_routine_safely(r, gaps)] += 1
            self.save()
        return done

    def save(self):
        if self.a.project:
            self.s.ok(cmd="ann_save", file=self.a.project)

    def report(self):
        """Remaining gaps after all passes (also written to --report)."""
        routines = self.only_filter(self.s.ok(cmd="routines")["routines"])
        rows = [(r, self.routine_gaps(r)) for r in routines]
        open_rows = [(r, g) for r, g in rows if g]
        low = sum(1 for r, _ in rows if r.get("confidence") == "low")
        self.say(f"gaps left: {len(open_rows)} of {len(rows)} routines" + (f" ({low} with low confidence)" if low else ""))
        if self.a.report:
            lines = ["# Annotation report", "", f"{len(rows) - len(open_rows)} of {len(rows)} routines complete, {len(open_rows)} with gaps.", ""]
            for r, g in open_rows:
                lines.append(f'## {r["name"]} ({r["addr"]}, {self.own_bytes(r)} bytes, confidence: {r.get("confidence") or "n/a"})')
                lines += [f"- {x}" for x in g]
                lines.append("")
            with open(self.a.report, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
        return len(open_rows)

    def dry_run(self):
        """What a real run would do, without contacting a model: routines in processing order with sizes and prompt estimates, jump tables,
        data regions, KERNAL calls. Lets you check the load address, the entry points and the model's context window first."""
        s, say = self.s, self.say
        say("")
        say("== dry run: no model is contacted ==")
        routines = s.ok(cmd="routines")["routines"]
        todo = s.ok(cmd="routines", undocumented_only=True)["routines"]
        order = {r["addr"]: i for i, r in enumerate(todo, 1)}
        base_tokens = int(len(SYSTEM_PROMPT) / 3.5)
        rows, total, largest = [], 0, (0, "")
        for r in routines:
            chars = sum(len(s.ok(cmd="listing", verify=False, equates=False, addresses=True, max_chars=14000, **{"from": bs, "to": be})["listing"])
                        for bs, be in self.blocks(r))
            tokens = int(chars / 3.5) + base_tokens
            if r["addr"] in order:
                total += tokens
                largest = max(largest, (tokens, r["name"]))
            rows.append((order.get(r["addr"]), r, tokens, self.own_bytes(r)))
        say(f'{len(routines)} routines, {len(todo)} still without documentation. Processing order (leaves first):')
        for pos, r, tokens, size in sorted(rows, key=lambda x: (x[0] is None, x[0] or 0)):
            kc = len(r.get("kernal_calls", []))
            extra = ""
            if r.get("interrupt_vector"):
                extra += f'  interrupt handler ({r["interrupt_vector"]})'
            elif r.get("via_table"):
                extra += "  via jump table"
            say(f'  {str(pos) + ".":>4} {r["name"]:<22} {r["addr"]}  {size:>5} B  {r["instructions"]:>4} instr  calls {len(r["callees"]) + len(r["external_calls"]):>2}'
                f'  called by {len(r["callers"]):>2}' + (f'  KERNAL calls {kc}' if kc else "") + extra + f'  ~{tokens} tokens' + ("  (documented)" if pos is None else ""))
        tables = s.ok(cmd="jump_tables", max_entries=0)["tables"]
        for t in tables:
            say(f'jump table ({t["kind"]}) at {t["table"]}: {t["entry_count"]} entries' + ("" if t["followed"] else " - suggested, not followed"))
        regions = [x for x in s.ok(cmd="data_regions")["regions"] if x["kind"] not in ("zeros", "basic")]  # these need no annotation
        if regions:
            kinds = Counter(x["kind"] for x in regions)
            say("data not yet annotated: " + ", ".join(f"{n} {k}" for k, n in sorted(kinds.items())))
        kernal = Counter(k["name"] for r in routines for k in r.get("kernal_calls", []))
        if kernal:
            say("KERNAL calls: " + ", ".join(f"{name} x{n}" for name, n in sorted(kernal.items())))
        io = Counter(x["name"] or x["addr"] for r in routines for kind in ("reads", "writes") for x in r.get("io_access", {}).get(kind, []))
        if io:
            say("chip registers used: " + ", ".join(f"{name}" for name, _ in sorted(io.items())))
        say(f'estimate: about {total} prompt tokens IN TOTAL for the first messages of all {len(todo)} routines to be processed '
            f'(the largest, {largest[1]}, about {largest[0]}); the conversation grows with each reply (typically 3-8 replies per routine, at most {self.a.max_turns}).')
        ctx = self.a.num_ctx
        if largest[0] > ctx * 0.4:
            say(f'WARNING: the largest routine ({largest[1]}, ~{largest[0]} tokens) takes more than 40% of the context window ({ctx}); '
                f'raise --num-ctx (and check that the model supports it) or accept that replies may be cut short.')
        say("Run the same command without --dry-run to start the analysis.")

    def run(self):
        s = self.s
        for n, p in enumerate(self.passes, 1):
            self.llm, self.mode, self.max_turns = p["llm"], p["mode"], p["max_turns"]
            self.said_thinking = False
            self.say(f'== pass {n}/{len(self.passes)} ({self.mode}): model {self.llm.model}' + (", thinking" if self.llm.think else "")
                     + ("" if getattr(self.llm, "vision", True) else ", no vision: screenshots are replaced by text") + " ==")
            result = self.explore_pass() if self.mode == "explore" else self.refine_pass()
            self.say(f"pass {n}: {result[True]} routines finished, {result['partial']} partial (documented, not finished), {result[False]} failed")
        listing = s.ok(cmd="listing", verify=True, max_chars=10_000_000)
        if self.a.out:
            with open(self.a.out, "w", encoding="utf-8") as f:
                f.write(listing["listing"])
        self.report()
        self.say("listing " + ("VERIFIED (re-assembles to the identical bytes)" if listing["verified"] else f'NOT verified: {listing.get("problem", "?")}')
                 + (f" -> {self.a.out}" if self.a.out else ""))
        return listing


def make_pass(a, model, mode, think, max_tokens, max_turns):
    """Creates the Ollama client for one pass and adapts it to what the model can do."""
    llm = Ollama(a.host, model, think=False, num_ctx=a.num_ctx, temperature=a.temperature, max_tokens=max_tokens)
    caps = llm.capabilities()
    if caps is not None and "tools" not in caps:
        raise OllamaError(f"model {model} does not support tool calling (capabilities: {', '.join(sorted(caps))})")
    llm.think = (caps is not None and "thinking" in caps) if think == "auto" else (think is True or think == "true")
    llm.vision = caps is None or "vision" in caps
    return {"llm": llm, "mode": mode, "max_turns": max_turns}


def find_m6510tool(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "build", "m6510tool"), os.path.join(here, "..", "build", "Release", "m6510tool.exe")):
        if os.path.exists(cand):
            return cand
    found = shutil.which("m6510tool")
    if not found:
        sys.exit("m6510tool not found: build the project or pass --m6510tool")
    return found


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--prg", help="C64 program (PRG or P00; load address from the file)")
    src.add_argument("--bin", help="raw binary (use with --org)")
    p.add_argument("--org", help='load address of --bin, or a different one for --prg (e.g. "0xC000")')
    p.add_argument("--machine", choices=["c64", "generic"], default="c64", help="c64: memory map, chips, KERNAL emulation, screen; generic: 64 KiB RAM")
    p.add_argument("--no-auto-tables", action="store_true", help="do not mark and follow tables of code addresses that the analysis only suggests")
    p.add_argument("--no-auto-data", action="store_true", help="do not mark the text, screen codes, sprites etc. that the analysis recognises as data regions")
    p.add_argument("--dry-run", action="store_true", help="load the program and run the analysis, print what a real run would do (routines, order, tables, data, token estimates), contact no model")
    p.add_argument("--entry", action="append", help='entry point (repeatable), default: the SYS address of a BASIC starter, else the load address')
    p.add_argument("--project", help="annotation file (JSON); loaded if present, saved after every routine")
    p.add_argument("--out", help="write the final annotated listing here")
    p.add_argument("--only", action="append", help="process only this routine address (repeatable)")
    p.add_argument("--model", default="qwen3-vl:8b-instruct", help="Ollama model tag for pass 1 (needs tool support; vision is detected)")
    p.add_argument("--refine-model", action="append", default=[], metavar="MODEL",
                   help="model for one more pass that checks and completes routines that still have gaps (repeatable; e.g. a slower thinking model)")
    p.add_argument("--refine-think", choices=["auto", "true", "false"], default="auto", help="thinking mode in refinement passes (auto: if the model has it)")
    p.add_argument("--refine-max-tokens", type=int, default=12000, help="answer limit in refinement passes (they may think a lot)")
    p.add_argument("--refine-max-turns", type=int, default=25, help="tool-call rounds per routine in refinement passes")
    p.add_argument("--refine-all", action="store_true", help="let the refinement passes review every documented routine, not only those with gaps")
    p.add_argument("--refine-medium", action="store_true", help="also revisit routines the model itself rated medium confidence")
    p.add_argument("--report", help="write the remaining gaps (per routine) to this Markdown file")
    p.add_argument("--host", default=os.environ.get("OLLAMA_HOST", "http://localhost:11434"))
    p.add_argument("--think", choices=["true", "false"], default="false",
                   help="the model's thinking mode (default false: tool calling works better and it cannot run away; ignored by models without it)")
    p.add_argument("--num-ctx", type=int, default=32768, help="context window in tokens (listing + conversation + images)")
    p.add_argument("--max-tokens", type=int, default=4096, help="limit for ONE model answer (with --think true use more, e.g. 12000)")
    p.add_argument("--temperature", type=float, default=0.7, help="do not go low: Qwen models repeat themselves endlessly at low temperature")
    p.add_argument("--lang", default="English", help='language of summaries and comments, e.g. "German"')
    p.add_argument("--max-turns", type=int, default=20, help="model replies per routine (several tool calls fit in one reply)")
    p.add_argument("--log", help="write every model message and tool result to this JSON-lines file")
    p.add_argument("--m6510tool", help="path to the m6510tool executable")
    return p


def main(argv=None):
    a = build_parser().parse_args(argv)
    if not a.host.startswith("http"):
        a.host = "http://" + a.host

    server = M6510Server(find_m6510tool(a.m6510tool))
    log = open(a.log, "a", encoding="utf-8") if a.log else None
    try:
        if a.dry_run:
            worker = Annotator(server, [], a, log)
            worker.setup()
            worker.dry_run()
            return 0
        passes = [make_pass(a, a.model, "explore", think=a.think == "true", max_tokens=a.max_tokens, max_turns=a.max_turns)]
        for model in a.refine_model:
            passes.append(make_pass(a, model, "refine", think=a.refine_think, max_tokens=a.refine_max_tokens, max_turns=a.refine_max_turns))
        worker = Annotator(server, passes, a, log)
        worker.setup()
        listing = worker.run()
        return 0 if listing["verified"] else 1
    except (ServerError, OllamaError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        server.close()
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
