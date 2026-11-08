#include "m6510/session.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "m6510/assembler.hpp"
#include "m6510/decoder.hpp"
#include "m6510/expression.hpp"
#include "m6510/format.hpp"
#include "m6510/image_io.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

struct SessionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr std::uint64_t kDefaultLimit = 1'000'000;
constexpr std::uint64_t kMaxLimit = 100'000'000;
constexpr std::size_t kMaxRead = 4096;
constexpr std::size_t kMaxWrite = 65536;
constexpr std::size_t kMaxTrace = 1000;
constexpr Address kDefaultReturn = 0xFFF0;

std::string hex2(unsigned v) { return "0x" + hexDigits(v & 0xFF, 2); }
std::string hex4(unsigned v) { return "0x" + hexDigits(v & 0xFFFF, 4); }

std::string hexBytes(const std::vector<Byte>& b) {
    std::string s;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i) s += ' ';
        s += hexDigits(b[i], 2);
    }
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string flagsText(Byte p) {
    const char* names = "NV-BDIZC";
    std::string s;
    for (int bit = 7; bit >= 0; --bit) s += bit == 5 ? '-' : ((p >> bit) & 1) ? names[7 - bit] : '.';
    return s;
}

const char* stopName(StopReason r) {
    switch (r) {
        case StopReason::None: return "stepped";
        case StopReason::Limit: return "instruction_limit";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Watchpoint: return "watchpoint";
        case StopReason::Reached: return "address";
        case StopReason::Returned: return "returned";
        case StopReason::Halted: return "halted";
        case StopReason::Requested: return "exit";
    }
    return "?";
}

std::vector<Byte> parseHex(const std::string& text) {
    std::string digits;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw SessionError("bad hex digit '" + std::string(1, c) + "'");
        digits += c;
    }
    if (digits.size() % 2) throw SessionError("odd number of hex digits");
    std::vector<Byte> out;
    for (std::size_t i = 0; i < digits.size(); i += 2) out.push_back(static_cast<Byte>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    return out;
}

}  // namespace

// ---- request handling ------------------------------------------------------------------------------
struct Session::Impl {
    Session& s;
    const Json& req;

    const Json* arg(const char* key) const { return req.find(key); }
    bool has(const char* key) const { return arg(key) && !arg(key)->isNull(); }

    std::int64_t value(const Json& v, const std::string& what) const {
        if (v.isNumber()) return v.asInt();
        if (v.isBool()) throw SessionError(what + ": expected a number or an expression, not a boolean");
        if (!v.isString()) throw SessionError(what + ": expected a number or an expression");
        const Registers& r = s.m_.cpu().registers();
        const ExpressionEvaluator ev(
            [&](const std::string& name, std::int64_t& out) {
                const std::string u = upper(name);
                if (s.m_.symbols.count(name) == 0) {  // registers by name unless a symbol has that name
                    if (u == "A") { out = r.a; return true; }
                    if (u == "X") { out = r.x; return true; }
                    if (u == "Y") { out = r.y; return true; }
                    if (u == "S" || u == "SP") { out = r.s; return true; }
                    if (u == "P") { out = r.p; return true; }
                    if (u == "PC") { out = r.pc; return true; }
                }
                return s.m_.lookupSymbol(name, out);
            },
            r.pc);
        const ExprResult x = ev.evaluate(v.asString());
        if (!x.ok) throw SessionError(what + ": " + x.error);
        if (x.undefined) throw SessionError(what + ": unknown symbol '" + x.undefinedName + "'");
        return x.value;
    }
    Address address(const char* key) const {
        const Json* v = arg(key);
        if (!v || v->isNull()) throw SessionError(std::string("missing '") + key + "'");
        const std::int64_t a = value(*v, key);
        return static_cast<Address>(a & 0xFFFF);
    }
    Address addressOr(const char* key, Address def) const { return has(key) ? address(key) : def; }
    std::int64_t number(const char* key, std::int64_t def) const { return has(key) ? value(*arg(key), key) : def; }
    std::string text(const char* key) const {
        const Json* v = arg(key);
        if (!v || !v->isString()) throw SessionError(std::string("missing string '") + key + "'");
        return v->asString();
    }
    bool flag(const char* key, bool def) const {
        const Json* v = arg(key);
        if (!v || v->isNull()) return def;
        if (!v->isBool()) throw SessionError(std::string("'") + key + "' must be true or false");
        return v->asBool();
    }
    std::uint64_t limit() const {
        const std::int64_t n = number("max_instructions", static_cast<std::int64_t>(kDefaultLimit));
        if (n < 0 || static_cast<std::uint64_t>(n) > kMaxLimit) {
            throw SessionError("max_instructions must be 0.." + std::to_string(kMaxLimit));
        }
        return static_cast<std::uint64_t>(n);
    }
    void needFiles() const {
        if (!s.options_.allowFiles) throw SessionError("file access is disabled (start the server with --allow-files)");
    }
    void needC64(const char* cmd) const {
        if (!s.m_.c64()) throw SessionError(std::string(cmd) + " needs the C64 machine (machine type c64)");
    }

    Byte peek(Address a) const { return s.m_.peek(a); }
    void poke(Address a, Byte v) const { s.m_.poke(a, v); }

    Json regs() const {
        const Cpu& c = s.m_.cpu();
        const Registers& r = c.registers();
        Json j = Json::object();
        j.set("A", hex2(r.a)).set("X", hex2(r.x)).set("Y", hex2(r.y)).set("S", hex2(r.s)).set("PC", hex4(r.pc));
        j.set("P", hex2(r.p)).set("flags", flagsText(r.p)).set("halted", c.halted());
        j.set("instructions", c.instructionCount()).set("cycles", c.cycleCount());
        return j;
    }

    Json runReply(const RunResult& rr, std::uint64_t interruptsBefore) const {
        Json j = Json::object();
        j.set("ok", true).set("stop", stopName(rr.reason)).set("instructions", rr.instructions).set("cycles", rr.cycles);
        j.set("pc", hex4(s.m_.cpu().registers().pc));
        j.set("interrupts", s.m_.cpu().interruptCount() - interruptsBefore);
        if (rr.reason == StopReason::Watchpoint) j.set("watch", Json::object().set("addr", hex4(rr.watchAddress)));
        if (rr.reason == StopReason::Requested) j.set("message", s.m_.cpu().stopMessage());
        j.set("regs", regs());
        return j;
    }

    std::map<Address, std::string> reverseSymbols() const { return s.m_.reverseSymbols(); }

    // ---- commands ----------------------------------------------------------------------------------
    Json run() {
        const Json* c = arg("cmd");
        if (!c || !c->isString()) throw SessionError("missing \"cmd\"");
        const std::string& cmd = c->asString();
        if (cmd == "help") return help();
        if (cmd == "asm") return cmdAsm();
        if (cmd == "dis") return cmdDis();
        if (cmd == "read") return cmdRead();
        if (cmd == "write") return cmdWrite();
        if (cmd == "load_file") return cmdLoadFile();
        if (cmd == "load_rom") return cmdLoadRom();
        if (cmd == "attach") return cmdAttach();
        if (cmd == "regs") return ok().set("regs", regs());
        if (cmd == "set") return cmdSet();
        if (cmd == "step" || cmd == "step_over" || cmd == "run" || cmd == "run_until_return") return cmdRun(cmd);
        if (cmd == "call") return cmdCall();
        if (cmd == "run_program") return cmdRunProgram();
        if (cmd == "break" || cmd == "unbreak") {
            const Address a = address("addr");
            if (cmd == "break") s.m_.cpu().addBreakpoint(a);
            else s.m_.cpu().removeBreakpoint(a);
            return ok();
        }
        if (cmd == "breaks" || cmd == "clear_breaks") return cmdBreaks(cmd);
        if (cmd == "watch" || cmd == "unwatch") return cmdWatch(cmd);
        if (cmd == "checkpoint" || cmd == "rollback" || cmd == "drop" || cmd == "checkpoints") return cmdCheckpoint(cmd);
        if (cmd == "diff") return cmdDiff();
        if (cmd == "state") return ok().set("state", stateJson());
        if (cmd == "set_state") return cmdSetState();
        if (cmd == "save_state" || cmd == "load_state") return cmdStateFile(cmd);
        if (cmd == "interrupt") return cmdInterrupt();
        if (cmd == "io") return cmdIo();
        if (cmd == "kernalcalls") return cmdKernalCalls();
        if (cmd == "console") {
            needC64("console");
            return ok().set("text", s.m_.kernal() ? s.m_.kernal()->takeConsole() : std::string());
        }
        if (cmd == "keys") {
            needC64("keys");
            if (!s.m_.kernal()) throw SessionError("keys needs the KERNAL emulation");
            s.m_.kernal()->addKeys(text("text"));
            return ok();
        }
        if (cmd == "screen") return cmdScreen();
        if (cmd == "screenshot" || cmd == "screens" || cmd == "screen_drop" || cmd == "screen_diff") return cmdScreenshot(cmd);
        if (cmd == "reset") return cmdReset();
        if (cmd == "symbols") {
            Json j = Json::object();
            for (const auto& [n, v] : s.m_.symbols) j.set(n, hex4(static_cast<unsigned>(v)));
            return ok().set("symbols", j);
        }
        if (cmd == "machine") return cmdMachine();
        throw SessionError("unknown command '" + cmd + "' (try {\"cmd\":\"help\"})");
    }

    static Json ok() { return Json::object().set("ok", true); }

    Json help() const {
        Json list = Json::array();
        for (const char* c : {"asm", "dis", "read", "write", "load_file", "load_rom", "attach", "regs", "set", "step",
                              "step_over", "run", "run_until_return", "call", "run_program", "break", "unbreak", "breaks",
                              "clear_breaks", "watch", "unwatch", "checkpoint", "rollback", "drop", "checkpoints", "diff",
                              "state", "set_state", "save_state", "load_state", "interrupt", "io", "kernalcalls", "console",
                              "keys", "screen", "screenshot", "screens", "screen_drop", "screen_diff", "reset", "symbols", "machine", "help"})
            list.push(c);
        return ok().set("commands", list).set("docs", "docs/agent-protocol.md");
    }

    Json cmdAsm() {
        AssemblerOptions o;
        o.origin = addressOr("origin", 0xC000);
        o.predefined = s.m_.assemblerSymbols();
        o.level = CpuLevel::Stable;
        const AssemblyResult r = assemble(text("source"), o);
        if (!r.ok()) {
            Json errors = Json::array();
            std::vector<std::string> lines;
            {
                std::string src = text("source"), line;
                for (char ch : src) {
                    if (ch == '\n') {
                        lines.push_back(line);
                        line.clear();
                    } else {
                        line += ch;
                    }
                }
                lines.push_back(line);
            }
            for (const Diagnostic& d : r.errors) {
                Json e = Json::object().set("line", d.line).set("message", d.message);
                if (d.line >= 1 && static_cast<std::size_t>(d.line) <= lines.size()) e.set("source", lines[d.line - 1]);
                errors.push(e);
            }
            return Json::object().set("ok", false).set("error", "assembly failed").set("errors", errors);
        }
        Json segs = Json::array();
        for (const Segment& seg : r.segments) {
            s.m_.memory().load(seg.address, seg.bytes);
            segs.push(Json::object().set("start", hex4(seg.address)).set("size", seg.bytes.size()));
        }
        Json syms = Json::object();
        for (const auto& [n, v] : r.symbols) {
            if (o.predefined.count(n)) continue;
            s.m_.symbols[n] = v;
            syms.set(n, hex4(static_cast<unsigned>(v)));
        }
        Json j = ok();
        const std::vector<Byte> flat = r.flatten();
        j.set("size", flat.size());
        if (!flat.empty()) j.set("start", hex4(r.lowest())).set("end", hex4(r.end() - 1));
        j.set("segments", segs).set("symbols", syms);
        if (has("entry")) {
            s.m_.cpu().registers().pc = address("entry");
            s.m_.cpu().setHalted(false);
            j.set("entry", hex4(s.m_.cpu().registers().pc));
        }
        return j;
    }

    Json cmdDis() {
        const Address start = addressOr("addr", s.m_.cpu().registers().pc);
        const std::int64_t count = number("count", 8);
        if (count < 1 || count > 1000) throw SessionError("count must be 1..1000");
        Memory view;
        for (unsigned i = 0; i < static_cast<unsigned>(count) * 3 + 3; ++i) {
            const auto a = static_cast<Address>(start + i);
            view.write8(a, peek(a));
        }
        const std::map<Address, std::string> names = reverseSymbols();
        DecodeOptions o;
        o.level = CpuLevel::All;
        o.symbolizer = [&names](Address a) {
            auto it = names.find(a);
            return it == names.end() ? std::string() : it->second;
        };
        Json lines = Json::array();
        Address a = start;
        for (std::int64_t i = 0; i < count; ++i) {
            const Instruction in = decode(view, a, o);
            Json l = Json::object().set("addr", hex4(a));
            if (auto it = names.find(a); it != names.end()) l.set("label", it->second);
            l.set("bytes", hexBytes(in.bytes)).set("text", in.text());
            if (in.target) l.set("target", hex4(*in.target));
            lines.push(l);
            a = static_cast<Address>(a + in.length());
        }
        return ok().set("lines", lines);
    }

    Json cmdRead() {
        const Address a = address("addr");
        const std::int64_t len = number("len", 16);
        if (len < 1 || static_cast<std::size_t>(len) > kMaxRead) throw SessionError("len must be 1.." + std::to_string(kMaxRead));
        const std::string view = has("view") ? text("view") : "cpu";
        if (view != "cpu" && view != "ram") throw SessionError("view must be cpu or ram");
        std::vector<Byte> b;
        for (std::int64_t i = 0; i < len; ++i) {
            const auto x = static_cast<Address>(a + i);
            b.push_back(view == "ram" ? s.m_.memory().read8(x) : peek(x));
        }
        std::string ascii, pet;
        for (Byte c : b) {
            ascii += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
            pet += screenCodeToChar(c);
        }
        return ok().set("addr", hex4(a)).set("hex", hexBytes(b)).set("ascii", ascii).set("screen", pet);
    }

    Json cmdWrite() {
        const Address a = address("addr");
        std::vector<Byte> b;
        if (has("hex")) {
            b = parseHex(text("hex"));
        } else if (const Json* arr = arg("bytes"); arr && arr->isArray()) {
            for (const Json& v : arr->items()) {
                const std::int64_t x = value(v, "bytes");
                if (x < -128 || x > 255) throw SessionError("byte value out of range");
                b.push_back(static_cast<Byte>(x));
            }
        } else {
            throw SessionError("write needs 'hex' or 'bytes'");
        }
        if (b.size() > kMaxWrite) throw SessionError("at most 65536 bytes per write");
        for (std::size_t i = 0; i < b.size(); ++i) poke(static_cast<Address>(a + i), b[i]);
        return ok().set("written", b.size());
    }

    Json cmdLoadFile() {
        needFiles();
        const std::string type = has("type") ? text("type") : "auto";
        const std::optional<Address> at = has("addr") ? std::optional<Address>(address("addr")) : std::nullopt;
        const LoadedFile f = s.m_.loadFile(text("file"), type, at);
        const Program& p = f.program;
        Json j = ok().set("start", hex4(p.address)).set("end", hex4(p.bytes.empty() ? p.address : p.end() - 1));
        if (!f.basic.empty()) j.set("basic", f.basic);
        j.set("entry", hex4(f.entry));
        return j;
    }

    Json cmdLoadRom() {
        needFiles();
        needC64("load_rom");
        s.m_.loadRom(text("which"), readBinaryFile(text("file")));
        return ok().set("kernal", s.m_.kernal() ? "emulated" : "rom");
    }

    Json cmdAttach() {
        needFiles();
        needC64("attach");
        if (!s.m_.kernal()) throw SessionError("attach needs the KERNAL emulation");
        const std::string path = text("file");
        s.m_.attach(path);
        return ok().set("attached", path);
    }

    Json cmdSet() {
        const Json* r = arg("regs");
        if (!r || !r->isObject()) throw SessionError("set needs 'regs' (an object)");
        Registers& g = s.m_.cpu().registers();
        for (const auto& [name, v] : r->members()) {
            const std::string u = upper(name);
            const std::int64_t x = value(v, name);
            if (u == "PC") {
                g.pc = static_cast<Address>(x & 0xFFFF);
                s.m_.cpu().setHalted(false);
                continue;
            }
            if (x < -128 || x > 255) throw SessionError(name + ": value out of range for an 8-bit register");
            const auto b = static_cast<Byte>(x);
            if (u == "A") g.a = b;
            else if (u == "X") g.x = b;
            else if (u == "Y") g.y = b;
            else if (u == "S" || u == "SP") g.s = b;
            else if (u == "P") {
                g.p = static_cast<Byte>(b | kFlagU);
                CpuState st = s.m_.cpu().state();  // a new I flag applies at once (no CLI/SEI delay)
                st.irqInhibit = g.flag(kFlagI);
                s.m_.cpu().setState(st);
            }
            else throw SessionError("unknown register '" + name + "' (A, X, Y, S, PC, P)");
        }
        return ok().set("regs", regs());
    }

    Json cmdRun(const std::string& cmd) {
        Cpu& c = s.m_.cpu();
        const std::uint64_t irqs = c.interruptCount();
        if (cmd == "step") {
            const std::int64_t n = number("n", 1);
            if (n < 0 || static_cast<std::uint64_t>(n) > kMaxLimit) throw SessionError("n must be 0.." + std::to_string(kMaxLimit));
            const bool trace = flag("trace", false);
            Json t = Json::array();
            RunResult rr;
            DecodeOptions o;
            o.level = CpuLevel::All;
            for (std::int64_t i = 0; i < n; ++i) {
                const Address pc = c.registers().pc;
                Instruction in;
                if (trace && static_cast<std::size_t>(i) < kMaxTrace) {
                    Memory view;
                    for (unsigned k = 0; k < 3; ++k) view.write8(static_cast<Address>(pc + k), peek(static_cast<Address>(pc + k)));
                    in = decode(view, pc, o);
                }
                const StepResult st = c.step();
                ++rr.instructions;
                rr.cycles += st.cycles;
                if (trace && static_cast<std::size_t>(i) < kMaxTrace) {
                    t.push(Json::object()
                               .set("addr", hex4(pc))
                               .set("bytes", st.interrupt ? std::string() : hexBytes(in.bytes))
                               .set("text", st.interrupt ? std::string("(interrupt)") : in.text())
                               .set("cycles", st.cycles));
                }
                if (st.reason == StopReason::Halted) {
                    rr.reason = StopReason::Halted;
                    break;
                }
            }
            Json j = runReply(rr, irqs);
            if (trace) j.set("trace", t);
            return j;
        }
        if (cmd == "step_over") return runReply(c.stepOver(limit()), irqs);
        if (cmd == "run_until_return") return runReply(c.runUntilReturn(limit()), irqs);
        if (has("until")) return runReply(c.runUntil(address("until"), limit()), irqs);
        return runReply(c.run(limit()), irqs);
    }

    void setRegsFrom(const Json* r) {
        if (!r) return;
        if (!r->isObject()) throw SessionError("'regs' must be an object");
        Json fake = Json::object().set("cmd", "set").set("regs", *r);
        Impl{s, fake}.cmdSet();
    }

    Json cmdCall() {
        const Address entry = address("addr");
        const Address ret = addressOr("return_address", kDefaultReturn);
        setRegsFrom(arg("regs"));
        if (const Json* m = arg("memory"); m && !m->isNull()) {
            if (!m->isObject()) throw SessionError("'memory' must be an object {\"address\": \"hex bytes\"}");
            for (const auto& [k, v] : m->members()) {
                const Address a = static_cast<Address>(value(Json(k), "memory address") & 0xFFFF);
                const std::vector<Byte> b = parseHex(v.isString() ? v.asString() : throw SessionError("memory values are hex strings"));
                for (std::size_t i = 0; i < b.size(); ++i) poke(static_cast<Address>(a + i), b[i]);
            }
        }
        const std::uint64_t irqs = s.m_.cpu().interruptCount();
        const Byte s0 = s.m_.cpu().registers().s;
        const RunResult rr = s.m_.cpu().call(entry, ret, limit());
        Json j = runReply(rr, irqs);
        const bool returned = rr.reason == StopReason::Returned;
        j.set("returned", returned);
        if (!returned && rr.reason == StopReason::Reached) {
            j.set("note", "the PC reached the return address, but the stack pointer is " + hex2(s.m_.cpu().registers().s) +
                              " instead of " + hex2(s0) + ": this is not a return");
        }
        return j;
    }

    Json cmdRunProgram() {
        needC64("run_program");
        if (!s.m_.kernal()) throw SessionError("run_program needs the KERNAL emulation");
        Address entry = 0;
        if (has("addr")) {
            entry = address("addr");
        } else if (auto t = findSysTarget(s.m_.memory(), 0x0801)) {
            entry = *t;
        } else {
            throw SessionError("no BASIC starter with SYS at $0801: give 'addr'");
        }
        s.m_.kernal()->startProgram(entry);
        const std::uint64_t irqs = s.m_.cpu().interruptCount();
        Json j = runReply(s.m_.cpu().run(limit()), irqs);
        j.set("entry", hex4(entry));
        return j;
    }

    Json cmdBreaks(const std::string& cmd) {
        if (cmd == "clear_breaks") {
            s.m_.cpu().clearBreakpoints();
            s.m_.clearWatches();
            return ok();
        }
        Json b = Json::array(), w = Json::array();
        for (Address a : s.m_.cpu().breakpoints()) b.push(hex4(a));
        for (const WatchSpec& x : s.m_.watches()) {
            w.push(Json::object().set("addr", hex4(x.start)).set("len", x.length).set(
                "kind", x.read && x.write ? "access" : x.read ? "read" : "write"));
        }
        return ok().set("breakpoints", b).set("watchpoints", w);
    }

    Json cmdWatch(const std::string& cmd) {
        const Address a = address("addr");
        if (cmd == "unwatch") {
            s.m_.removeWatch(a);
            return ok();
        }
        const std::int64_t len = number("len", 1);
        if (len < 1 || len > 0x10000) throw SessionError("len must be 1..65536");
        const std::string kind = has("kind") ? text("kind") : "write";
        if (kind != "write" && kind != "read" && kind != "access") throw SessionError("kind must be write, read or access");
        s.m_.addWatch({a, static_cast<unsigned>(len), kind != "write", kind != "read"});
        return ok();
    }

    Json cmdCheckpoint(const std::string& cmd) {
        if (cmd == "checkpoints") {
            Json l = Json::array();
            for (const auto& [n, snap] : s.m_.checkpoints) l.push(n);
            return ok().set("checkpoints", l);
        }
        const std::string name = text("name");
        if (cmd == "checkpoint") {
            s.m_.checkpoints[name] = s.m_.capture();
            return ok();
        }
        auto it = s.m_.checkpoints.find(name);
        if (it == s.m_.checkpoints.end()) throw SessionError("no checkpoint '" + name + "'");
        if (cmd == "drop") s.m_.checkpoints.erase(it);
        else s.m_.restore(it->second);
        return ok();
    }

    Json cmdDiff() {
        const std::string name = text("name");
        auto it = s.m_.checkpoints.find(name);
        if (it == s.m_.checkpoints.end()) throw SessionError("no checkpoint '" + name + "'");
        std::size_t total = 0;
        std::vector<MemoryChange> changes = diffBytes(it->second.memory.raw(), s.m_.memory().raw(), 0, 8, 50, &total);
        std::size_t colorTotal = 0;
        if (s.m_.c64() && it->second.c64) {
            const C64::State now = s.m_.c64()->state();
            const std::vector<Byte> a(it->second.c64->color.begin(), it->second.c64->color.end());
            const std::vector<Byte> b(now.color.begin(), now.color.end());
            for (MemoryChange& c : diffBytes(a, b, 0xD800, 8, 50, &colorTotal)) changes.push_back(std::move(c));
        }
        Json ranges = Json::array();
        for (const MemoryChange& c : changes) {
            if (ranges.items().size() >= 50) break;
            const std::size_t shown = std::min<std::size_t>(c.before.size(), 64);
            Json r = Json::object().set("addr", hex4(c.address)).set("len", c.before.size());
            r.set("before", hexBytes(std::vector<Byte>(c.before.begin(), c.before.begin() + static_cast<std::ptrdiff_t>(shown))));
            r.set("after", hexBytes(std::vector<Byte>(c.after.begin(), c.after.begin() + static_cast<std::ptrdiff_t>(shown))));
            if (c.address >= 0xD800 && c.address < 0xDC00 && colorTotal > 0 && s.m_.c64()) r.set("area", "color");
            ranges.push(r);
        }
        return ok().set("changed_bytes", total + colorTotal).set("ranges", ranges);
    }

    Json stateJson() const {
        const CpuState c = s.m_.cpu().state();
        Json j = regs();
        j.set("irq_line", c.irqLine).set("nmi_pending", c.nmiPending).set("interrupts", c.interrupts).set("depth", c.depth);
        j.set("port_enabled", c.portEnabled).set("port_direction", hex2(c.portDirection)).set("port_data", hex2(c.portData));
        return j;
    }

    Json cmdSetState() {
        const Json* st = arg("state");
        if (!st || !st->isObject()) throw SessionError("set_state needs 'state' (an object)");
        Json regsOnly = Json::object();
        for (const char* k : {"A", "X", "Y", "S", "P", "PC"})
            if (const Json* v = st->find(k)) regsOnly.set(k, *v);
        setRegsFrom(&regsOnly);
        CpuState c = s.m_.cpu().state();
        if (const Json* v = st->find("halted"); v && v->isBool()) c.halted = v->asBool();
        if (const Json* v = st->find("irq_line"); v && v->isBool()) c.irqLine = v->asBool();
        if (const Json* v = st->find("nmi_pending"); v && v->isBool()) c.nmiPending = v->asBool();
        if (const Json* v = st->find("port_direction")) c.portDirection = static_cast<Byte>(value(*v, "port_direction"));
        if (const Json* v = st->find("port_data")) c.portData = static_cast<Byte>(value(*v, "port_data"));
        s.m_.cpu().setState(c);
        return ok().set("state", stateJson());
    }

    Json cmdStateFile(const std::string& cmd) {
        needFiles();
        const std::string file = text("file");
        if (cmd == "save_state") {
            writeBinaryFile(file, serialize(s.m_.capture()));
            return ok();
        }
        s.m_.restore(deserializeSnapshot(readBinaryFile(file)));
        return ok().set("regs", regs());
    }

    Json cmdInterrupt() {
        const std::string kind = has("kind") ? text("kind") : "irq";
        const bool release = flag("release", false);
        if (kind == "nmi") {
            s.m_.cpu().nmi();
        } else if (kind == "irq") {
            if (s.m_.c64()) s.m_.c64()->setExternalIrq(!release);
            else s.m_.cpu().setIrq(!release);
        } else {
            throw SessionError("kind must be irq or nmi");
        }
        return ok();
    }

    Json cmdIo() {
        needC64("io");
        Json out = Json::array();
        for (const IoWrite& w : s.m_.c64()->takeIoLog()) {
            Json e = Json::object().set("addr", hex4(w.address)).set("value", hex2(w.value)).set("pc", hex4(w.pc)).set("chip", w.chip);
            if (!w.name.empty()) e.set("what", w.name);
            out.push(e);
        }
        return ok().set("output", out);
    }

    Json cmdKernalCalls() {
        needC64("kernalcalls");
        Json out = Json::array();
        if (s.m_.kernal()) {
            for (const KernalCall& c : s.m_.kernal()->takeCalls()) {
                auto rj = [](const Registers& r) {
                    return Json::object().set("A", hex2(r.a)).set("X", hex2(r.x)).set("Y", hex2(r.y)).set("flags", flagsText(r.p));
                };
                Json e = Json::object().set("addr", hex4(c.address)).set("name", c.name).set("emulated", c.emulated);
                e.set("call_site", hex4(c.callSite)).set("regs_in", rj(c.before)).set("regs_out", rj(c.after));
                if (!c.note.empty()) e.set("note", c.note);
                out.push(e);
            }
        }
        return ok().set("calls", out);
    }

    Json cmdScreen() {
        needC64("screen");
        const C64& m = (*s.m_.c64());
        const Address screen = m.screenAddress();
        Json lines = Json::array();
        for (const std::string& l : s.m_.screenLines()) lines.push(l);
        const Byte d011 = m.vic(0x11);
        const std::string mode = s.m_.screenMode();
        Json j = ok().set("lines", lines).set("mode", mode).set("screen", hex4(screen)).set("bank", hex4(m.vicBank()));
        j.set("border", m.vic(0x20) & 0x0F).set("background", m.vic(0x21) & 0x0F).set("display_on", (d011 & 0x10) != 0);
        j.set("memory", m.describeConfig());
        return j;
    }

    Json cmdScreenshot(const std::string& cmd) {
        if (cmd == "screens") {
            Json l = Json::array();
            for (const auto& [n, img] : s.screens_) l.push(Json::object().set("name", n).set("width", img.width).set("height", img.height));
            return ok().set("screens", l);
        }
        if (cmd == "screen_drop") {
            if (s.screens_.erase(text("name")) == 0) throw SessionError("no screen '" + text("name") + "'");
            return ok();
        }
        needC64(cmd.c_str());
        RenderOptions ro;
        ro.border = flag("border", false);
        if (has("palette") && !parsePalette(text("palette"), ro.palette)) throw SessionError("palette must be colodore or pepto");
        const IndexedImage current = renderScreen(*s.m_.c64(), s.m_.memory(), ro);
        if (cmd == "screen_diff") {
            const auto a = s.screens_.find(text("a"));
            if (a == s.screens_.end()) throw SessionError("no screen '" + text("a") + "'");
            const IndexedImage* other = &current;
            if (has("b")) {
                const auto b = s.screens_.find(text("b"));
                if (b == s.screens_.end()) throw SessionError("no screen '" + text("b") + "'");
                other = &b->second;
            }
            const ImageDiff d = diffImages(a->second, *other);
            Json j = ok().set("same_size", d.sameSize).set("changed_pixels", d.changedPixels);
            if (d.sameSize && d.changedPixels > 0) {
                j.set("bbox", Json::object().set("x0", d.x0).set("y0", d.y0).set("x1", d.x1).set("y1", d.y1));
                const int off = other->width > 320 ? 32 : 0, offy = other->height > 200 ? 36 : 0;
                auto cellX = [&](int x) { return std::clamp((x - off) / 8, 0, 39); };
                auto cellY = [&](int y) { return std::clamp((y - offy) / 8, 0, 24); };
                j.set("text_cells", Json::object().set("col0", cellX(d.x0)).set("row0", cellY(d.y0)).set("col1", cellX(d.x1)).set("row1", cellY(d.y1)));
            }
            return j;
        }
        const std::string name = has("name") ? text("name") : "last";
        s.screens_[name] = current;  // the full screen is stored; crop/zoom only change what is returned
        IndexedImage shown = current;
        Json j = ok();
        const int off = ro.border ? 32 : 0, offy = ro.border ? 36 : 0;
        if (const Json* crop = arg("crop"); crop && !crop->isNull()) {
            if (!crop->isObject()) throw SessionError("crop must be an object {col0,row0,col1,row1} (text cells)");
            auto cell = [&](const char* key, int max) {
                const Json* v = crop->find(key);
                if (!v || !v->isNumber() || v->asInt() < 0 || v->asInt() > max)
                    throw SessionError(std::string("crop needs ") + key + " (0.." + std::to_string(max) + ")");
                return static_cast<int>(v->asInt());
            };
            const int col0 = cell("col0", 39), row0 = cell("row0", 24), col1 = cell("col1", 39), row1 = cell("row1", 24);
            if (col1 < col0 || row1 < row0) throw SessionError("crop: end is before start");
            const int px = (col1 - col0 + 1) * 8;
            const std::int64_t zoom = number("zoom", std::clamp(320 / px, 1, 8));
            if (zoom < 1 || zoom > 16) throw SessionError("zoom must be 1..16");
            shown = cropAndZoom(current, off + col0 * 8, offy + row0 * 8, off + col1 * 8 + 7, offy + row1 * 8 + 7, static_cast<int>(zoom));
            j.set("zoom", zoom);
        } else if (has("zoom")) {
            const std::int64_t zoom = number("zoom", 1);
            if (zoom < 1 || zoom > 4) throw SessionError("zoom must be 1..4 for the whole screen");
            shown = cropAndZoom(current, 0, 0, current.width - 1, current.height - 1, static_cast<int>(zoom));
            j.set("zoom", zoom);
        }
        j.set("name", name).set("mode", s.m_.screenMode()).set("width", shown.width).set("height", shown.height);
        j.set("charset", s.m_.c64()->hasChar() ? "rom" : "replacement");
        j.set("png_base64", base64Encode(encodePng(shown)));
        return j;
    }

    Json cmdReset() {
        s.m_.reset(flag("memory", false));
        return ok().set("regs", regs());
    }

    Json cmdMachine() {
        const std::string type = has("type") ? text("type") : "generic";
        const std::string kernal = has("kernal") ? text("kernal") : "hle";
        if (type != "generic" && type != "c64") throw SessionError("machine type must be generic or c64");
        if (kernal != "hle" && kernal != "rom") throw SessionError("kernal must be hle or rom");
        s.m_.select(type == "c64", kernal == "hle");
        Json j = ok().set("machine", type);
        if (type == "c64") j.set("kernal", s.m_.kernal() ? "emulated" : "rom (load it with load_rom)").set("memory", s.m_.c64()->describeConfig());
        return j;
    }
};

// ---- Session ------------------------------------------------------------------------------------------
Session::Session(SessionOptions options) : options_(options) {}

Session::~Session() = default;

Json Session::handle(const Json& request) {
    try {
        if (!request.isObject()) throw SessionError("a request must be a JSON object");
        Impl impl{*this, request};
        return impl.run();
    } catch (const std::exception& e) {
        return Json::object().set("ok", false).set("error", e.what());
    }
}

std::string Session::handleLine(std::string_view line) {
    Json reply;
    try {
        reply = handle(Json::parse(line));
    } catch (const std::exception& e) {
        reply = Json::object().set("ok", false).set("error", e.what());
    }
    return reply.dump();
}

}  // namespace m6510
