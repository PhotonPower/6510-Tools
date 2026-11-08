#include "m6510/session.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

// Screen code -> printable character for the text view of the screen.
char screenChar(Byte sc) {
    sc = static_cast<Byte>(sc & 0x7F);
    if (sc == 0) return '@';
    if (sc <= 26) return static_cast<char>('A' + sc - 1);
    if (sc == 27) return '[';
    if (sc == 29) return ']';
    if (sc >= 32 && sc < 64) return static_cast<char>(sc);
    return '.';
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
        const Registers& r = s.cpu_->registers();
        const ExpressionEvaluator ev(
            [&](const std::string& name, std::int64_t& out) {
                if (auto it = s.symbols_.find(name); it != s.symbols_.end()) {
                    out = it->second;
                    return true;
                }
                const std::string u = upper(name);
                if (u == "A") out = r.a;
                else if (u == "X") out = r.x;
                else if (u == "Y") out = r.y;
                else if (u == "S" || u == "SP") out = r.s;
                else if (u == "P") out = r.p;
                else if (u == "PC") out = r.pc;
                else {
                    if (s.c64_) {
                        for (const auto& [a, n] : Kernal::jumpTable()) {
                            if (n == u) {
                                out = a;
                                return true;
                            }
                        }
                    }
                    return false;
                }
                return true;
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
        if (!s.c64_) throw SessionError(std::string(cmd) + " needs the C64 machine (machine type c64)");
    }

    Byte peek(Address a) const { return s.c64_ ? s.c64_->peek(a) : s.cpu_->peek(a); }
    void poke(Address a, Byte v) const {
        if (s.c64_) s.c64_->write(a, v);
        else s.memory_->write8(a, v);
    }

    Json regs() const {
        const Cpu& c = *s.cpu_;
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
        j.set("pc", hex4(s.cpu_->registers().pc));
        j.set("interrupts", s.cpu_->interruptCount() - interruptsBefore);
        if (rr.reason == StopReason::Watchpoint) j.set("watch", Json::object().set("addr", hex4(rr.watchAddress)));
        if (rr.reason == StopReason::Requested) j.set("message", s.cpu_->stopMessage());
        j.set("regs", regs());
        return j;
    }

    std::map<Address, std::string> reverseSymbols() const {
        std::map<Address, std::string> names;
        if (s.c64_)
            for (const auto& [a, n] : Kernal::jumpTable()) names[a] = n;
        for (const auto& [n, v] : s.symbols_) {
            if (v < 0 || v > 0xFFFF) continue;
            auto it = names.find(static_cast<Address>(v));
            if (it == names.end() || it->second.find('.') != std::string::npos) names[static_cast<Address>(v)] = n;
        }
        return names;
    }

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
            if (cmd == "break") s.cpu_->addBreakpoint(a);
            else s.cpu_->removeBreakpoint(a);
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
            return ok().set("text", s.kernal_ ? s.kernal_->takeConsole() : std::string());
        }
        if (cmd == "keys") {
            needC64("keys");
            if (!s.kernal_) throw SessionError("keys needs the KERNAL emulation");
            s.kernal_->addKeys(text("text"));
            return ok();
        }
        if (cmd == "screen") return cmdScreen();
        if (cmd == "reset") return cmdReset();
        if (cmd == "symbols") {
            Json j = Json::object();
            for (const auto& [n, v] : s.symbols_) j.set(n, hex4(static_cast<unsigned>(v)));
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
                              "keys", "screen", "reset", "symbols", "machine", "help"})
            list.push(c);
        return ok().set("commands", list).set("docs", "docs/agent-protocol.md");
    }

    Json cmdAsm() {
        AssemblerOptions o;
        o.origin = addressOr("origin", 0xC000);
        o.predefined = s.symbols_;
        if (s.c64_)
            for (const auto& [addr, n] : Kernal::jumpTable()) o.predefined.emplace(n, addr);  // JSR CHROUT
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
            s.memory_->load(seg.address, seg.bytes);
            segs.push(Json::object().set("start", hex4(seg.address)).set("size", seg.bytes.size()));
        }
        Json syms = Json::object();
        for (const auto& [n, v] : r.symbols) {
            if (o.predefined.count(n)) continue;
            s.symbols_[n] = v;
            syms.set(n, hex4(static_cast<unsigned>(v)));
        }
        Json j = ok();
        const std::vector<Byte> flat = r.flatten();
        j.set("size", flat.size());
        if (!flat.empty()) j.set("start", hex4(r.lowest())).set("end", hex4(r.end() - 1));
        j.set("segments", segs).set("symbols", syms);
        if (has("entry")) {
            s.cpu_->registers().pc = address("entry");
            s.cpu_->setHalted(false);
            j.set("entry", hex4(s.cpu_->registers().pc));
        }
        return j;
    }

    Json cmdDis() {
        const Address start = addressOr("addr", s.cpu_->registers().pc);
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
            b.push_back(view == "ram" ? s.memory_->read8(x) : peek(x));
        }
        std::string ascii, pet;
        for (Byte c : b) {
            ascii += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
            pet += screenChar(c);
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
        const std::string file = text("file");
        std::string type = has("type") ? text("type") : "auto";
        const std::vector<Byte> data = readBinaryFile(file);
        if (type == "auto") {
            const std::string ext = fileExtension(file);
            if (looksLikeP00(data)) type = "p00";
            else if (ext == ".bin" || ext == ".raw") type = "raw";
            else if (ext == ".d64" || ext == ".t64" || ext == ".crt") type = ext.substr(1);
            else type = "prg";
        }
        Program p;
        if (type == "raw") {
            p.address = address("addr");
            p.bytes = data;
            if (p.end() > 0x10000) throw SessionError("raw file does not fit between addr and $FFFF");
        } else if (type == "prg") {
            p = parsePrg(data);
            if (has("addr")) p.address = address("addr");
        } else if (type == "p00") {
            p = parseP00(data);
            if (has("addr")) p.address = address("addr");
        } else if (type == "d64" || type == "t64" || type == "crt") {
            throw SessionError(type + " images are planned for milestone 0.6 (docs/roadmap.md)");
        } else {
            throw SessionError("unknown type '" + type + "' (auto, raw, prg, p00)");
        }
        if (p.end() > 0x10000) throw SessionError("program does not fit below $FFFF");
        loadProgram(*s.memory_, p);
        Address entry = p.address;
        Json j = ok().set("start", hex4(p.address)).set("end", hex4(p.bytes.empty() ? p.address : p.end() - 1));
        if (auto t = findSysTarget(*s.memory_, p.address); t && p.address == 0x0801) {
            entry = *t;
            std::string error;
            const auto lines = detokenizeBasic(*s.memory_, p.address, &error, 3);
            if (!lines.empty()) j.set("basic", std::to_string(lines[0].number) + " " + lines[0].text);
        }
        j.set("entry", hex4(entry));
        s.cpu_->registers().pc = entry;
        s.cpu_->setHalted(false);
        return j;
    }

    Json cmdLoadRom() {
        needFiles();
        needC64("load_rom");
        const std::string which = text("which");
        s.c64_->loadRom(which, readBinaryFile(text("file")));
        if (which == "kernal") {
            s.kernal_.reset();  // the real KERNAL replaces the emulation
            s.cpu_->reset();
        }
        return ok().set("kernal", s.kernal_ ? "emulated" : "rom");
    }

    Json cmdAttach() {
        needFiles();
        needC64("attach");
        if (!s.kernal_) throw SessionError("attach needs the KERNAL emulation");
        const std::string path = text("file");
        namespace fs = std::filesystem;
        if (!fs::exists(path)) throw SessionError("'" + path + "' does not exist");
        s.attached_ = path;
        s.kernal_->files = [path](const std::string& name, unsigned device) -> std::optional<Program> {
            if (device < 8) return std::nullopt;
            try {
                if (fs::is_directory(path)) {
                    std::vector<fs::path> prgs;
                    for (const auto& e : fs::directory_iterator(path)) {
                        if (e.is_regular_file() && (fileExtension(e.path().string()) == ".prg" || fileExtension(e.path().string()) == ".p00"))
                            prgs.push_back(e.path());
                    }
                    std::sort(prgs.begin(), prgs.end());
                    for (const fs::path& f : prgs) {
                        const std::string stem = upper(f.stem().string());
                        if (name == "*" || name.empty() || stem == upper(name)) return loadProgramFile(f.string());
                    }
                    return std::nullopt;
                }
                return loadProgramFile(path);  // a single file answers every name
            } catch (const IoError&) {
                return std::nullopt;
            }
        };
        return ok().set("attached", path);
    }

    Json cmdSet() {
        const Json* r = arg("regs");
        if (!r || !r->isObject()) throw SessionError("set needs 'regs' (an object)");
        Registers& g = s.cpu_->registers();
        for (const auto& [name, v] : r->members()) {
            const std::string u = upper(name);
            const std::int64_t x = value(v, name);
            if (u == "PC") {
                g.pc = static_cast<Address>(x & 0xFFFF);
                s.cpu_->setHalted(false);
                continue;
            }
            if (x < -128 || x > 255) throw SessionError(name + ": value out of range for an 8-bit register");
            const auto b = static_cast<Byte>(x);
            if (u == "A") g.a = b;
            else if (u == "X") g.x = b;
            else if (u == "Y") g.y = b;
            else if (u == "S" || u == "SP") g.s = b;
            else if (u == "P") g.p = static_cast<Byte>(b | kFlagU);
            else throw SessionError("unknown register '" + name + "' (A, X, Y, S, PC, P)");
        }
        return ok().set("regs", regs());
    }

    Json cmdRun(const std::string& cmd) {
        Cpu& c = *s.cpu_;
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
        const std::uint64_t irqs = s.cpu_->interruptCount();
        const Byte s0 = s.cpu_->registers().s;
        const RunResult rr = s.cpu_->call(entry, ret, limit());
        Json j = runReply(rr, irqs);
        const bool returned = rr.reason == StopReason::Returned;
        j.set("returned", returned);
        if (!returned && rr.reason == StopReason::Reached) {
            j.set("note", "the PC reached the return address, but the stack pointer is " + hex2(s.cpu_->registers().s) +
                              " instead of " + hex2(s0) + ": this is not a return");
        }
        return j;
    }

    Json cmdRunProgram() {
        needC64("run_program");
        if (!s.kernal_) throw SessionError("run_program needs the KERNAL emulation");
        Address entry = 0;
        if (has("addr")) {
            entry = address("addr");
        } else if (auto t = findSysTarget(*s.memory_, 0x0801)) {
            entry = *t;
        } else {
            throw SessionError("no BASIC starter with SYS at $0801: give 'addr'");
        }
        s.kernal_->startProgram(entry);
        const std::uint64_t irqs = s.cpu_->interruptCount();
        Json j = runReply(s.cpu_->run(limit()), irqs);
        j.set("entry", hex4(entry));
        return j;
    }

    Json cmdBreaks(const std::string& cmd) {
        if (cmd == "clear_breaks") {
            s.cpu_->clearBreakpoints();
            s.watches_.clear();
            s.applyWatches();
            return ok();
        }
        Json b = Json::array(), w = Json::array();
        for (Address a : s.cpu_->breakpoints()) b.push(hex4(a));
        for (const Watch& x : s.watches_) {
            w.push(Json::object().set("addr", hex4(x.start)).set("len", x.length).set(
                "kind", x.read && x.write ? "access" : x.read ? "read" : "write"));
        }
        return ok().set("breakpoints", b).set("watchpoints", w);
    }

    Json cmdWatch(const std::string& cmd) {
        const Address a = address("addr");
        if (cmd == "unwatch") {
            s.watches_.erase(std::remove_if(s.watches_.begin(), s.watches_.end(), [a](const Watch& w) { return w.start == a; }),
                             s.watches_.end());
            s.applyWatches();
            return ok();
        }
        const std::int64_t len = number("len", 1);
        if (len < 1 || len > 0x10000) throw SessionError("len must be 1..65536");
        const std::string kind = has("kind") ? text("kind") : "write";
        if (kind != "write" && kind != "read" && kind != "access") throw SessionError("kind must be write, read or access");
        s.watches_.push_back({a, static_cast<unsigned>(len), kind != "write", kind != "read"});
        s.applyWatches();
        return ok();
    }

    Json cmdCheckpoint(const std::string& cmd) {
        if (cmd == "checkpoints") {
            Json l = Json::array();
            for (const auto& [n, snap] : s.checkpoints_) l.push(n);
            return ok().set("checkpoints", l);
        }
        const std::string name = text("name");
        if (cmd == "checkpoint") {
            s.checkpoints_[name] = s.capture();
            return ok();
        }
        auto it = s.checkpoints_.find(name);
        if (it == s.checkpoints_.end()) throw SessionError("no checkpoint '" + name + "'");
        if (cmd == "drop") s.checkpoints_.erase(it);
        else s.restore(it->second);
        return ok();
    }

    Json cmdDiff() {
        const std::string name = text("name");
        auto it = s.checkpoints_.find(name);
        if (it == s.checkpoints_.end()) throw SessionError("no checkpoint '" + name + "'");
        std::size_t total = 0;
        std::vector<MemoryChange> changes = diffBytes(it->second.memory.raw(), s.memory_->raw(), 0, 8, 50, &total);
        std::size_t colorTotal = 0;
        if (s.c64_ && it->second.c64) {
            const C64::State now = s.c64_->state();
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
            if (c.address >= 0xD800 && c.address < 0xDC00 && colorTotal > 0 && s.c64_) r.set("area", "color");
            ranges.push(r);
        }
        return ok().set("changed_bytes", total + colorTotal).set("ranges", ranges);
    }

    Json stateJson() const {
        const CpuState c = s.cpu_->state();
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
        CpuState c = s.cpu_->state();
        if (const Json* v = st->find("halted"); v && v->isBool()) c.halted = v->asBool();
        if (const Json* v = st->find("irq_line"); v && v->isBool()) c.irqLine = v->asBool();
        if (const Json* v = st->find("nmi_pending"); v && v->isBool()) c.nmiPending = v->asBool();
        if (const Json* v = st->find("port_direction")) c.portDirection = static_cast<Byte>(value(*v, "port_direction"));
        if (const Json* v = st->find("port_data")) c.portData = static_cast<Byte>(value(*v, "port_data"));
        s.cpu_->setState(c);
        return ok().set("state", stateJson());
    }

    Json cmdStateFile(const std::string& cmd) {
        needFiles();
        const std::string file = text("file");
        if (cmd == "save_state") {
            writeBinaryFile(file, serialize(s.capture()));
            return ok();
        }
        const Snapshot snap = deserializeSnapshot(readBinaryFile(file));
        if (snap.c64.has_value() != (s.c64_ != nullptr)) {
            throw SessionError(std::string("the snapshot is for the ") + (snap.c64 ? "c64" : "generic") + " machine; select it first");
        }
        s.restore(snap);
        return ok().set("regs", regs());
    }

    Json cmdInterrupt() {
        const std::string kind = has("kind") ? text("kind") : "irq";
        const bool release = flag("release", false);
        if (kind == "nmi") {
            s.cpu_->nmi();
        } else if (kind == "irq") {
            if (s.c64_) s.c64_->setExternalIrq(!release);
            else s.cpu_->setIrq(!release);
        } else {
            throw SessionError("kind must be irq or nmi");
        }
        return ok();
    }

    Json cmdIo() {
        needC64("io");
        Json out = Json::array();
        for (const IoWrite& w : s.c64_->takeIoLog()) {
            Json e = Json::object().set("addr", hex4(w.address)).set("value", hex2(w.value)).set("pc", hex4(w.pc)).set("chip", w.chip);
            if (!w.name.empty()) e.set("what", w.name);
            out.push(e);
        }
        return ok().set("output", out);
    }

    Json cmdKernalCalls() {
        needC64("kernalcalls");
        Json out = Json::array();
        if (s.kernal_) {
            for (const KernalCall& c : s.kernal_->takeCalls()) {
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
        const C64& m = *s.c64_;
        const Address screen = m.screenAddress();
        Json lines = Json::array();
        for (unsigned row = 0; row < 25; ++row) {
            std::string l;
            for (unsigned col = 0; col < 40; ++col) l += screenChar(s.memory_->read8(static_cast<Address>(screen + row * 40 + col)));
            while (!l.empty() && l.back() == ' ') l.pop_back();
            lines.push(l);
        }
        const Byte d011 = m.vic(0x11), d016 = m.vic(0x16);
        std::string mode = (d011 & 0x20) ? ((d016 & 0x10) ? "multicolor bitmap" : "hires bitmap")
                                         : (d011 & 0x40) ? "extended background text" : (d016 & 0x10) ? "multicolor text" : "text";
        Json j = ok().set("lines", lines).set("mode", mode).set("screen", hex4(screen)).set("bank", hex4(m.vicBank()));
        j.set("border", m.vic(0x20) & 0x0F).set("background", m.vic(0x21) & 0x0F).set("display_on", (d011 & 0x10) != 0);
        j.set("memory", m.describeConfig());
        return j;
    }

    Json cmdReset() {
        const bool clear = flag("memory", false);
        if (clear) s.memory_->clear();
        CpuState c = s.cpu_->state();
        c.instructions = c.cycles = c.interrupts = 0;
        s.cpu_->setState(c);
        if (s.c64_) {
            s.c64_->resetChips();
            if (s.kernal_) {
                s.kernal_->coldStart();
                s.cpu_->registers() = Registers{};
                s.cpu_->registers().pc = Kernal::kExitTrap;
            } else {
                s.cpu_->powerOn();
            }
        } else {
            s.cpu_->powerOn();
            s.cpu_->registers().s = 0xFF;
        }
        return ok().set("regs", regs());
    }

    Json cmdMachine() {
        const std::string type = has("type") ? text("type") : "generic";
        const std::string kernal = has("kernal") ? text("kernal") : "hle";
        if (type != "generic" && type != "c64") throw SessionError("machine type must be generic or c64");
        if (kernal != "hle" && kernal != "rom") throw SessionError("kernal must be hle or rom");
        s.selectMachine(type == "c64", kernal == "hle");
        Json j = ok().set("machine", type);
        if (type == "c64") j.set("kernal", s.kernal_ ? "emulated" : "rom (load it with load_rom)").set("memory", s.c64_->describeConfig());
        return j;
    }
};

// ---- Session ------------------------------------------------------------------------------------------
Session::Session(SessionOptions options) : options_(options) { selectMachine(false, false); }

Session::~Session() {
    kernal_.reset();
    c64_.reset();
}

void Session::selectMachine(bool c64, bool hle) {
    kernal_.reset();
    c64_.reset();
    cpu_.reset();
    memory_ = std::make_unique<Memory>();
    cpu_ = std::make_unique<Cpu>(*memory_);
    symbols_.clear();
    checkpoints_.clear();
    watches_.clear();
    attached_.clear();
    if (!c64) {
        cpu_->setPortEnabled(false);  // generic machine: 64 KiB RAM everywhere, $00/$01 are RAM
        cpu_->powerOn();
        cpu_->registers().s = 0xFF;
        return;
    }
    c64_ = std::make_unique<C64>(*memory_, *cpu_);
    if (hle) {
        kernal_ = std::make_unique<Kernal>(*c64_, *cpu_, *memory_);
        kernal_->install();
        cpu_->powerOn();
        c64_->resetChips();
        kernal_->coldStart();
        (void)c64_->takeIoLog();
        cpu_->registers().pc = Kernal::kExitTrap;
    } else {
        cpu_->powerOn();
        c64_->resetChips();
    }
}

Snapshot Session::capture() const {
    Snapshot snap;
    snap.cpu = cpu_->state();
    snap.memory = *memory_;
    if (c64_) snap.c64 = c64_->state();
    if (kernal_) snap.kernal = kernal_->state();
    return snap;
}

void Session::restore(const Snapshot& snap) {
    *memory_ = snap.memory;
    cpu_->setState(snap.cpu);
    if (c64_ && snap.c64) c64_->setState(*snap.c64);
    if (kernal_ && snap.kernal) kernal_->setState(*snap.kernal);
}

void Session::applyWatches() {
    cpu_->clearWatchpoints();
    for (const Watch& w : watches_) cpu_->addWatchpoint(w.start, w.length, w.read, w.write);
}

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
