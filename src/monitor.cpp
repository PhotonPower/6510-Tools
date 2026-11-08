#include "m6510/monitor.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

#include "m6510/assembler.hpp"
#include "m6510/decoder.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/expression.hpp"
#include "m6510/format.hpp"
#include "m6510/image_io.hpp"
#include "m6510/opcodes.hpp"
#include "m6510/petscii.hpp"
#include "m6510/version.hpp"
#include "m6510/video.hpp"

namespace m6510 {

namespace {

struct MonitorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string h2(unsigned v) { return hexDigits(v & 0xFF, 2); }
std::string h4(unsigned v) { return hexDigits(v & 0xFFFF, 4); }
std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}
std::string flagsText(Byte p) {
    const char* names = "NV-BDIZC";
    std::string s;
    for (int bit = 7; bit >= 0; --bit) s += bit == 5 ? '-' : ((p >> bit) & 1) ? names[7 - bit] : '.';
    return s;
}
std::string bytesText(const std::vector<Byte>& b, std::size_t max = 3) {
    std::string s;
    for (std::size_t i = 0; i < b.size() && i < max; ++i) s += (i ? " " : "") + h2(b[i]);
    if (b.size() > max) s += "..";
    return s;
}

// Splits a line into words; "quoted text" stays one word (quotes kept).
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (quoted) {
            cur += c;
            if (c == '"') quoted = false;
        } else if (c == '"') {
            cur += c;
            quoted = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool isQuoted(const std::string& s) { return s.size() >= 2 && s.front() == '"' && s.back() == '"'; }
std::string unquote(const std::string& s) { return isQuoted(s) ? s.substr(1, s.size() - 2) : s; }

}  // namespace

struct Monitor::Impl {
    Monitor& m;
    explicit Impl(Monitor& monitor) : m(monitor) {}

    Machine& mach() { return m.m_; }
    const Machine& mach() const { return m.m_; }
    Cpu& cpu() { return m.m_.cpu(); }
    Registers& regs() { return m.m_.cpu().registers(); }
    const Registers& regs() const { return m.m_.cpu().registers(); }

    // ---- numbers and expressions (hexadecimal by default) ----
    bool registerValue(const std::string& u, std::int64_t& v) const {
        const Registers& r = regs();
        if (u == "A") v = r.a;
        else if (u == "X") v = r.x;
        else if (u == "Y") v = r.y;
        else if (u == "S" || u == "SP") v = r.s;
        else if (u == "P") v = r.p;
        else if (u == "PC") v = r.pc;
        else return false;
        return true;
    }
    std::string substituteRegisters(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '@') {
                out += text[i];
                continue;
            }
            std::size_t j = i + 1;
            while (j < text.size() && std::isalnum(static_cast<unsigned char>(text[j]))) ++j;
            const std::string name = upper(text.substr(i + 1, j - i - 1));
            std::int64_t v = 0;
            if (!registerValue(name, v)) throw MonitorError("unknown register '@" + name + "' (@A @X @Y @S @P @PC)");
            out += "$" + hexDigits(static_cast<unsigned>(v), 1);
            i = j - 1;
        }
        return out;
    }
    std::int64_t value(const std::string& text) const {
        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& v) { return mach().lookupSymbol(n, v); },
                                     regs().pc, true);
        const ExprResult r = ev.evaluate(substituteRegisters(text));
        if (!r.ok) throw MonitorError("bad number or expression '" + text + "': " + r.error);
        if (r.undefined) throw MonitorError("unknown symbol '" + r.undefinedName + "'");
        return r.value;
    }
    Address address(const std::string& text) const { return static_cast<Address>(value(text) & 0xFFFF); }
    Byte byteValue(const std::string& text) const {
        const std::int64_t v = value(text);
        if (v < -128 || v > 255) throw MonitorError("'" + text + "' does not fit in a byte");
        return static_cast<Byte>(v);
    }
    std::int64_t count(const std::string& text, std::int64_t lo, std::int64_t hi) const {
        const std::int64_t n = value(text);
        if (n < lo || n > hi) throw MonitorError("count must be " + hexDigits(static_cast<unsigned>(lo), 1) + ".." +
                                                 hexDigits(static_cast<unsigned>(hi), 1) + " (hex)");
        return n;
    }

    // bytes from arguments: numbers and "text"; unit 2 = little-endian words
    std::vector<Byte> byteList(const std::vector<std::string>& args, std::size_t from, unsigned unit = 1) const {
        std::vector<Byte> out;
        for (std::size_t i = from; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (isQuoted(a)) {
                for (std::size_t k = 1; k + 1 < a.size(); ++k) out.push_back(static_cast<Byte>(a[k]));
                continue;
            }
            const std::int64_t v = value(a);
            if (unit == 1) {
                if (v < -128 || v > 255) throw MonitorError("'" + a + "' does not fit in a byte");
                out.push_back(static_cast<Byte>(v));
            } else {
                if (v < -32768 || v > 65535) throw MonitorError("'" + a + "' does not fit in a word");
                out.push_back(static_cast<Byte>(v & 0xFF));
                out.push_back(static_cast<Byte>((v >> 8) & 0xFF));
            }
        }
        if (out.empty()) throw MonitorError("no data given");
        return out;
    }

    // ---- output helpers ----
    Instruction decodeAt(Address a, const std::map<Address, std::string>& names) const {
        DecodeOptions opt;
        opt.level = CpuLevel::All;
        opt.symbolizer = [&names](Address x) {
            const auto it = names.find(x);
            return it == names.end() ? std::string() : it->second;
        };
        return decode(mach().view(a, 3), a, opt);
    }
    std::string instructionLine(Address a, const std::map<Address, std::string>& names, bool labels, Instruction* decoded = nullptr,
                                bool marker = true) const {
        const Instruction in = decodeAt(a, names);
        if (decoded) *decoded = in;
        std::string out;
        if (labels) {
            if (const auto it = names.find(a); it != names.end()) out += it->second + ":\n";
        }
        out += std::string(marker && a == regs().pc ? "> " : "  ") + h4(a) + "  " + pad(bytesText(in.bytes), 10) + in.text() + "\n";
        return out;
    }
    std::string currentLine() const { return instructionLine(regs().pc, mach().reverseSymbols(), true); }

    std::string regsText() const {
        const Registers& r = regs();
        const Cpu& c = mach().cpu();
        std::string out = " PC=" + h4(r.pc) + "  A=" + h2(r.a) + " X=" + h2(r.x) + " Y=" + h2(r.y) + "  S=" + h2(r.s) +
                          "  P=" + h2(r.p) + " " + flagsText(r.p);
        if (const C64* c64 = mach().c64()) out += "  $01=" + h2(c64->peek(1)) + " line " + std::to_string(c64->rasterLine());
        out += "  cycles " + std::to_string(c.cycleCount());
        if (c.halted()) out += "  HALTED (JAM)";
        return out + "\n";
    }

    std::string stopText(const RunResult& r) const {
        std::string o = "stopped: ";
        switch (r.reason) {
            case StopReason::Breakpoint: o += "breakpoint at " + h4(regs().pc); break;
            case StopReason::Watchpoint: o += "watchpoint " + h4(r.watchAddress) + " (instruction at " + h4(mach().cpu().instructionAddress()) + ")"; break;
            case StopReason::Reached: o += "reached " + h4(regs().pc); break;
            case StopReason::Returned: o += "returned"; break;
            case StopReason::Halted: o += "JAM at " + h4(mach().cpu().instructionAddress()) + " (processor halted)"; break;
            case StopReason::Requested: o += mach().cpu().stopMessage(); break;
            case StopReason::WrittenCode: o += "code in memory the program wrote, at " + h4(regs().pc); break;
            case StopReason::Limit: o += "instruction limit (" + std::to_string(m.limit_) + ")"; break;
            case StopReason::None: o += "stepped"; break;
        }
        return o + "  [" + std::to_string(r.instructions) + " instructions, " + std::to_string(r.cycles) + " cycles]\n";
    }
    std::string afterRun(const RunResult& r, std::uint64_t interruptsBefore) {
        std::string o = stopText(r);
        const std::uint64_t irqs = mach().cpu().interruptCount() - interruptsBefore;
        if (irqs) o += " interrupts taken: " + std::to_string(irqs) + "\n";
        o += regsText() + currentLine();
        m.nextDisassembly_ = regs().pc;
        return o;
    }

    void requireFiles() const {
        if (!m.options_.allowFiles) throw MonitorError("file commands are disabled (--no-files)");
    }
    C64& requireC64(const std::string& cmd) {
        if (!mach().c64()) throw MonitorError(cmd + " needs the C64 machine (machine c64)");
        return *mach().c64();
    }
    Kernal& requireKernal(const std::string& cmd) {
        requireC64(cmd);
        if (!mach().kernal()) throw MonitorError(cmd + " needs the KERNAL emulation (machine c64 kernal)");
        return *mach().kernal();
    }

    // ---- commands ----
    std::string run(const std::vector<std::string>& a);
    std::string assembleLine(const std::string& line);
    std::string cmdDisassemble(const std::vector<std::string>& a);
    std::string cmdDump(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdEdit(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdRegisters(const std::vector<std::string>& a);
    std::string cmdExecute(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdInterrupt(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdFiles(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdSymbols(const std::vector<std::string>& a);
    std::string cmdBreaks(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdMachine(const std::vector<std::string>& a);
    std::string cmdC64(const std::string& cmd, const std::vector<std::string>& a);
    static std::string help();
};

std::string Monitor::banner() {
    return std::string("m6510tool monitor ") + kVersion + " - MOS 6510 / C64 (type ? for help, q to quit; numbers are hexadecimal)\n";
}

Monitor::Monitor(MonitorOptions options) : options_(options), limit_(options.maxInstructions) {}

Monitor::~Monitor() = default;

std::string Monitor::prompt() const { return assembleAt_ ? hexDigits(*assembleAt_, 4) + ":" : "-"; }

std::string Monitor::execute(std::string_view line) {
    Impl impl(*this);
    try {
        if (assembleAt_) return impl.assembleLine(std::string(line));
        const std::vector<std::string> args = tokenize(line);
        if (args.empty()) return "";
        return impl.run(args);
    } catch (const std::exception& e) {
        return std::string("? ") + e.what() + "\n";
    }
}

std::string Monitor::Impl::help() {
    return "numbers are hexadecimal: C000 FF; 100. = decimal; $FF 0xFF %1010; symbols and expressions (start+3); @A @X @Y @S @P @PC\n"
           "  d [adr [n]]            disassemble (n instructions, default 10 hex; no address: continue)\n"
           "  m [adr [n]]            memory as hex + ASCII (n bytes, default 80 hex); m.s shows screen codes\n"
           "  e adr b...             write bytes (numbers and \"text\"); e.w writes words (little-endian)\n"
           "  f from to b...         fill (pattern repeated, 'to' inclusive)   c from to dest   compare\n"
           "  t from to dest         copy (overlap-safe)                       h from to b...   search bytes or \"text\"\n"
           "  a [adr]                assemble lines (assembler syntax: decimal, $ for hex); an empty line ends\n"
           "  r [name=value ...]     show / set registers: a x y s p pc (r pc=c000 a=@x)\n"
           "  n [count]              single step(s)       o   step over JSR       u   run until the routine returns\n"
           "  g [adr]                run until a breakpoint / watchpoint / JAM / limit       ru adr   run until PC == adr\n"
           "  b [adr...]             set breakpoints (no argument: list)       bc [adr...]   clear (no argument: all)\n"
           "  w adr [len] [r|w|rw]   watchpoint (default: 1 byte, write)       limit n   instructions per run\n"
           "  sym [name value | -name]   symbols        symload file / symsave file (assembler -s or VICE labels)\n"
           "  asm file [org]         assemble a source file into memory        l file [adr]   load PRG / P00 / raw\n"
           "  sv file from to [raw]  save memory as PRG (or raw), 'to' inclusive\n"
           "  ck name / rb name      checkpoint / roll back                    savestate file / loadstate file\n"
           "  irq [on|off] / nmi     interrupt (irq: take it now if I = 0; on/off: hold / release the line)\n"
           "  reset [mem]            reset (and clear memory)                  machine [generic | c64 [kernal|rom]]\n"
           "C64 (machine c64):\n"
           "  bank [value]           memory configuration ($01)                rom basic|kernal|char file\n"
           "  io                     writes to chip registers                  kernal   KERNAL calls\n"
           "  console                text printed through CHROUT               screen   text screen (40x25)\n"
           "  keys \"text\"            keys for GETIN/CHRIN                      run [adr]   start like RUN / SYS\n"
           "  shot file.png [col row w h] [zoom] [border] [pepto]   screen as PNG (box in text cells)\n"
           "  ?  help      q  quit\n";
}

std::string Monitor::Impl::run(const std::vector<std::string>& a) {
    const std::string cmd = lower(a[0]);
    if (cmd == "?" || cmd == "help") return help();
    if (cmd == "q" || cmd == "quit" || cmd == "x" || cmd == "exit") {
        m.finished_ = true;
        return "";
    }
    if (cmd == "d") return cmdDisassemble(a);
    if (cmd == "m" || cmd == "m.s") return cmdDump(cmd, a);
    if (cmd == "e" || cmd == "e.w" || cmd == "f" || cmd == "c" || cmd == "t" || cmd == "h") return cmdEdit(cmd, a);
    if (cmd == "a") {
        m.assembleAt_ = a.size() > 1 ? address(a[1]) : m.nextDisassembly_;
        return "";
    }
    if (cmd == "r") return cmdRegisters(a);
    if (cmd == "n" || cmd == "o" || cmd == "u" || cmd == "g" || cmd == "ru") return cmdExecute(cmd, a);
    if (cmd == "b" || cmd == "bc" || cmd == "w") return cmdBreaks(cmd, a);
    if (cmd == "limit") {
        if (a.size() < 2) return "limit " + hexDigits(static_cast<unsigned>(m.limit_), 1) + " (" + std::to_string(m.limit_) + ")\n";
        const std::int64_t n = value(a[1]);
        if (n < 1) throw MonitorError("limit must be at least 1");
        m.limit_ = static_cast<std::uint64_t>(n);
        return "limit " + std::to_string(m.limit_) + "\n";
    }
    if (cmd == "sym") return cmdSymbols(a);
    if (cmd == "symload" || cmd == "symsave" || cmd == "asm" || cmd == "l" || cmd == "sv" || cmd == "savestate" ||
        cmd == "loadstate" || cmd == "rom")
        return cmdFiles(cmd, a);
    if (cmd == "ck" || cmd == "rb") {
        if (a.size() < 2) throw MonitorError("usage: " + cmd + " name");
        if (cmd == "ck") {
            mach().checkpoints[a[1]] = mach().capture();
            return "";
        }
        const auto it = mach().checkpoints.find(a[1]);
        if (it == mach().checkpoints.end()) throw MonitorError("no checkpoint named '" + a[1] + "'");
        mach().restore(it->second);
        m.nextDisassembly_ = regs().pc;
        return regsText() + currentLine();
    }
    if (cmd == "irq" || cmd == "nmi") return cmdInterrupt(cmd, a);
    if (cmd == "reset") {
        mach().reset(a.size() > 1 && lower(a[1]) == "mem");
        m.nextDisassembly_ = regs().pc;
        return regsText();
    }
    if (cmd == "machine") return cmdMachine(a);
    if (cmd == "bank" || cmd == "io" || cmd == "kernal" || cmd == "console" || cmd == "screen" || cmd == "keys" ||
        cmd == "run" || cmd == "shot")
        return cmdC64(cmd, a);
    throw MonitorError("unknown command '" + a[0] + "' (? for help)");
}

std::string Monitor::Impl::cmdDisassemble(const std::vector<std::string>& a) {
    const Address from = a.size() > 1 ? address(a[1]) : m.nextDisassembly_;
    const std::int64_t n = a.size() > 2 ? count(a[2], 1, 0x1000) : 16;
    const std::map<Address, std::string> names = mach().reverseSymbols();
    std::string out;
    Address at = from;
    for (std::int64_t i = 0; i < n; ++i) {
        Instruction in;
        out += instructionLine(at, names, true, &in);
        at = static_cast<Address>(at + in.length());
    }
    m.nextDisassembly_ = at;
    return out;
}

std::string Monitor::Impl::cmdDump(const std::string& cmd, const std::vector<std::string>& a) {
    const bool screen = cmd == "m.s";
    const Address from = a.size() > 1 ? address(a[1]) : m.nextDump_;
    const std::int64_t n = a.size() > 2 ? count(a[2], 1, 0x10000) : 128;
    std::string out;
    for (std::int64_t off = 0; off < n; off += 16) {
        const auto len = static_cast<unsigned>(std::min<std::int64_t>(16, n - off));
        const auto base = static_cast<Address>(from + off);
        std::string hex, text;
        for (unsigned i = 0; i < len; ++i) {
            const Byte b = mach().peek(static_cast<Address>(base + i));
            hex += h2(b) + (i == 7 ? "  " : " ");
            text += screen ? screenCodeToChar(b) : (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
        }
        while (!hex.empty() && hex.back() == ' ') hex.pop_back();
        out += h4(base) + "  " + pad(hex, 48) + "  " + text + "\n";
    }
    m.nextDump_ = static_cast<Address>(from + n);
    return out;
}

std::string Monitor::Impl::cmdEdit(const std::string& cmd, const std::vector<std::string>& a) {
    if (cmd == "e" || cmd == "e.w") {
        if (a.size() < 3) throw MonitorError("usage: " + cmd + " address data...");
        const Address at = address(a[1]);
        const std::vector<Byte> data = byteList(a, 2, cmd == "e.w" ? 2 : 1);
        for (std::size_t i = 0; i < data.size(); ++i) mach().poke(static_cast<Address>(at + i), data[i]);
        m.nextDump_ = at;
        return "";
    }
    if (a.size() < 4) throw MonitorError("usage: " + cmd + (cmd == "f" || cmd == "h" ? " from to data..." : " from to dest"));
    const Address from = address(a[1]);
    const Address to = address(a[2]);
    if (to < from) throw MonitorError("'to' is below 'from'");
    const std::size_t len = static_cast<std::size_t>(to - from) + 1;
    if (cmd == "f") {
        const std::vector<Byte> pattern = byteList(a, 3);
        for (std::size_t i = 0; i < len; ++i) mach().poke(static_cast<Address>(from + i), pattern[i % pattern.size()]);
        return "";
    }
    if (cmd == "c") {
        const Address dest = address(a[3]);
        std::string out;
        std::size_t diffs = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const auto x = static_cast<Address>(from + i), y = static_cast<Address>(dest + i);
            const Byte bx = mach().peek(x), by = mach().peek(y);
            if (bx != by && diffs++ < 32) out += "  " + h4(x) + ": " + h2(bx) + "   " + h4(y) + ": " + h2(by) + "\n";
        }
        if (diffs > 32) out += "  ... " + std::to_string(diffs - 32) + " more\n";
        return diffs == 0 ? "identical\n" : out;
    }
    if (cmd == "t") {
        const Address dest = address(a[3]);
        std::vector<Byte> tmp;
        for (std::size_t i = 0; i < len; ++i) tmp.push_back(mach().peek(static_cast<Address>(from + i)));  // buffered: overlap-safe
        for (std::size_t i = 0; i < len; ++i) mach().poke(static_cast<Address>(dest + i), tmp[i]);
        return "";
    }
    // h: search
    const std::vector<Byte> needle = byteList(a, 3);
    std::string out;
    std::size_t hits = 0;
    for (std::size_t i = 0; i + needle.size() <= len; ++i) {
        bool same = true;
        for (std::size_t k = 0; same && k < needle.size(); ++k) same = mach().peek(static_cast<Address>(from + i + k)) == needle[k];
        if (same && hits++ < 64) out += "  " + h4(static_cast<Address>(from + i)) + "\n";
    }
    if (hits > 64) out += "  ... " + std::to_string(hits - 64) + " more\n";
    return hits == 0 ? "not found\n" : out;
}

std::string Monitor::Impl::cmdRegisters(const std::vector<std::string>& a) {
    if (a.size() == 1) return regsText();
    const CpuState before = cpu().state();
    bool pcChanged = false, pChanged = false;
    try {
        for (std::size_t i = 1; i < a.size(); ++i) {  // left to right: r a=5 x=@a sets X to 5
            const std::size_t eq = a[i].find('=');
            if (eq == std::string::npos || eq == 0) throw MonitorError("usage: r name=value ...  (a x y s p pc)");
            const std::string name = upper(a[i].substr(0, eq));
            const std::int64_t v = value(a[i].substr(eq + 1));
            Registers& r = regs();
            if (name == "PC") {
                r.pc = static_cast<Address>(v & 0xFFFF);
                pcChanged = true;
                continue;
            }
            if (v < -128 || v > 255) throw MonitorError(name + ": value does not fit in 8 bits");
            const auto b = static_cast<Byte>(v);
            if (name == "A") r.a = b;
            else if (name == "X") r.x = b;
            else if (name == "Y") r.y = b;
            else if (name == "S" || name == "SP") r.s = b;
            else if (name == "P") r.p = static_cast<Byte>(b | kFlagU), pChanged = true;
            else throw MonitorError("unknown register '" + a[i].substr(0, eq) + "' (a x y s p pc)");
        }
    } catch (...) {
        cpu().setState(before);  // all or nothing
        throw;
    }
    if (pChanged) {  // a new I flag applies at once (no CLI/SEI delay)
        CpuState st = cpu().state();
        st.irqInhibit = regs().flag(kFlagI);
        cpu().setState(st);
    }
    if (pcChanged) {
        cpu().setHalted(false);
        m.nextDisassembly_ = regs().pc;
    }
    return regsText();
}

std::string Monitor::Impl::cmdExecute(const std::string& cmd, const std::vector<std::string>& a) {
    Cpu& c = cpu();
    const std::uint64_t irqs = c.interruptCount();
    if (cmd == "n") {
        const std::int64_t n = a.size() > 1 ? count(a[1], 1, 0x1000000) : 1;
        const bool show = n >= 2 && n <= 0x20;
        const std::map<Address, std::string> names = show ? mach().reverseSymbols() : std::map<Address, std::string>{};
        std::string shown;
        RunResult r;
        for (std::int64_t i = 0; i < n; ++i) {
            if (show) shown += instructionLine(regs().pc, names, false, nullptr, false);
            const StepResult s = c.step();
            ++r.instructions;
            r.cycles += s.cycles;
            if (s.reason == StopReason::Halted) {
                r.reason = StopReason::Halted;
                break;
            }
        }
        return shown + afterRun(r, irqs);
    }
    if (cmd == "g" && a.size() > 1) {
        regs().pc = address(a[1]);
        c.setHalted(false);
    }
    RunResult r;
    if (cmd == "o") r = c.stepOver(m.limit_);
    else if (cmd == "u") r = c.runUntilReturn(m.limit_);
    else if (cmd == "ru") {
        if (a.size() < 2) throw MonitorError("usage: ru address");
        r = c.runUntil(address(a[1]), m.limit_);
    } else {
        r = c.run(m.limit_);
    }
    return afterRun(r, irqs);
}

std::string Monitor::Impl::cmdInterrupt(const std::string& cmd, const std::vector<std::string>& a) {
    Cpu& c = cpu();
    auto setLine = [this](bool on) {
        if (C64* c64 = mach().c64()) c64->setExternalIrq(on);
        else cpu().setIrq(on);
    };
    if (cmd == "irq" && a.size() > 1) {
        const std::string what = lower(a[1]);
        if (what != "on" && what != "off") throw MonitorError("usage: irq [on|off]");
        setLine(what == "on");
        return what == "on" ? "IRQ line held active (irq off releases it)\n" : "IRQ line released\n";
    }
    if (cmd == "irq") {
        if (regs().flag(kFlagI)) throw MonitorError("IRQs are disabled (I = 1); 'irq on' holds the line until they are enabled");
        setLine(true);
    } else {
        c.nmi();
    }
    // Right after CLI/SEI/PLP the processor runs one more instruction before it takes an IRQ.
    StepResult s = c.step();
    std::string note;
    if (!s.interrupt && s.reason != StopReason::Halted) {
        note = " (after one more instruction: interrupt delay of CLI/SEI/PLP)";
        s = c.step();
    }
    if (cmd == "irq") setLine(false);
    m.nextDisassembly_ = regs().pc;
    if (!s.interrupt) return "? the interrupt was not taken\n" + regsText() + currentLine();
    return std::string(cmd == "irq" ? "IRQ" : "NMI") + " taken, handler at " + h4(regs().pc) + note + "\n" + regsText() + currentLine();
}

std::string Monitor::Impl::cmdBreaks(const std::string& cmd, const std::vector<std::string>& a) {
    Cpu& c = cpu();
    if (cmd == "b") {
        if (a.size() == 1) {
            const std::map<Address, std::string> names = mach().reverseSymbols();
            std::string out;
            for (const Address x : c.breakpoints()) {
                const auto it = names.find(x);
                out += "  break " + h4(x) + (it == names.end() ? "" : "  (" + it->second + ")") + "\n";
            }
            for (const WatchSpec& w : mach().watches())
                out += "  watch " + h4(w.start) + " len " + hexDigits(w.length, 1) + (w.read && w.write ? " rw" : w.read ? " r" : " w") + "\n";
            return out.empty() ? "no breakpoints\n" : out;
        }
        for (std::size_t i = 1; i < a.size(); ++i) c.addBreakpoint(address(a[i]));
        return "";
    }
    if (cmd == "bc") {
        if (a.size() == 1) {
            c.clearBreakpoints();
            mach().clearWatches();
            return "";
        }
        for (std::size_t i = 1; i < a.size(); ++i) {
            const Address x = address(a[i]);
            c.removeBreakpoint(x);
            mach().removeWatch(x);
        }
        return "";
    }
    // w adr [len] [r|w|rw]
    if (a.size() < 2) throw MonitorError("usage: w address [length] [r|w|rw]");
    WatchSpec w;
    w.start = address(a[1]);
    for (std::size_t i = 2; i < a.size(); ++i) {
        const std::string t = lower(a[i]);
        if (t == "r") w.read = true, w.write = false;
        else if (t == "w") w.read = false, w.write = true;
        else if (t == "rw") w.read = w.write = true;
        else w.length = static_cast<unsigned>(count(a[i], 1, 0x10000));
    }
    mach().addWatch(w);
    return "";
}

std::string Monitor::Impl::cmdSymbols(const std::vector<std::string>& a) {
    if (a.size() == 1) {
        std::string out;
        for (const auto& [k, v] : mach().symbols) out += "  " + pad(k, 20) + " = $" + h4(static_cast<unsigned>(v)) + "\n";
        return out.empty() ? "no symbols\n" : out;
    }
    if (a[1][0] == '-') {
        if (mach().symbols.erase(a[1].substr(1)) == 0) throw MonitorError("no symbol '" + a[1].substr(1) + "'");
        return "";
    }
    if (a.size() < 3) throw MonitorError("usage: sym name value   |   sym -name");
    std::string why;
    if (!isValidSymbolName(a[1], &why)) throw MonitorError("'" + a[1] + "' cannot be a symbol: " + why);
    mach().symbols[a[1]] = value(a[2]);
    return "";
}

std::string Monitor::Impl::cmdMachine(const std::vector<std::string>& a) {
    if (a.size() == 1) {
        if (!mach().c64()) return "generic (64 KiB RAM, no I/O port)\n";
        return std::string("c64, KERNAL ") + (mach().kernal() ? "emulated" : mach().c64()->hasKernal() ? "ROM" : "missing") +
               ", " + mach().c64()->describeConfig() + "\n";
    }
    const std::string type = lower(a[1]);
    const std::string kernal = a.size() > 2 ? lower(a[2]) : "kernal";
    if (type != "generic" && type != "c64") throw MonitorError("unknown machine '" + a[1] + "' (generic, c64)");
    if (kernal != "kernal" && kernal != "rom") throw MonitorError("usage: machine c64 [kernal|rom]");
    mach().select(type == "c64", kernal == "kernal");
    m.nextDisassembly_ = regs().pc;
    m.nextDump_ = 0;
    return cmdMachine({"machine"});
}

std::string Monitor::Impl::cmdC64(const std::string& cmd, const std::vector<std::string>& a) {
    C64& c64 = requireC64(cmd);
    if (cmd == "shot") {
        // shot file [col row width height] [zoom] [border] [pepto]: text cells of the 40 x 25 screen
        requireFiles();
        if (a.size() < 2) throw MonitorError("usage: shot file.png [col row width height] [zoom] [border] [pepto]");
        RenderOptions ro;
        std::vector<std::int64_t> nums;
        for (std::size_t i = 2; i < a.size(); ++i) {
            const std::string w = lower(a[i]);
            if (w == "border") ro.border = true;
            else if (w == "pepto" || w == "colodore") (void)parsePalette(w, ro.palette);
            else nums.push_back(value(a[i]));
        }
        if (nums.size() != 0 && nums.size() != 1 && nums.size() != 4 && nums.size() != 5)
            throw MonitorError("usage: shot file.png [col row width height] [zoom] [border] [pepto]");
        IndexedImage img = renderScreen(c64, mach().memory(), ro);
        const int ox = ro.border ? 32 : 0, oy = ro.border ? 36 : 0;
        std::int64_t zoom = 1;
        if (nums.size() >= 4) {
            const std::int64_t col = nums[0], row = nums[1], w = nums[2], h = nums[3];
            if (col < 0 || row < 0 || w < 1 || h < 1 || col + w > 40 || row + h > 25)
                throw MonitorError("the box must lie inside the 40 x 25 text cells (numbers are hex: 28 = 40)");
            zoom = nums.size() == 5 ? nums[4] : std::clamp<std::int64_t>(320 / (w * 8), 1, 8);
            if (zoom < 1 || zoom > 16) throw MonitorError("zoom must be 1..16");
            img = cropAndZoom(img, ox + static_cast<int>(col) * 8, oy + static_cast<int>(row) * 8,
                              ox + static_cast<int>(col + w) * 8 - 1, oy + static_cast<int>(row + h) * 8 - 1, static_cast<int>(zoom));
        } else if (nums.size() == 1) {
            zoom = nums[0];
            if (zoom < 1 || zoom > 4) throw MonitorError("zoom must be 1..4 for the whole screen");
            img = cropAndZoom(img, 0, 0, img.width - 1, img.height - 1, static_cast<int>(zoom));
        }
        writeBinaryFile(unquote(a[1]), encodePng(img));
        return "ok: " + std::to_string(img.width) + " x " + std::to_string(img.height) + " pixels, " + mach().screenMode() +
               (c64.hasChar() ? "" : ", replacement character set") + "\n";
    }
    if (cmd == "bank") {
        if (a.size() > 1) {
            const Byte v = byteValue(a[1]);
            const Byte dir = cpu().portDirection();
            if ((dir & 7) != 7) mach().poke(0, static_cast<Byte>(dir | 7));  // LORAM/HIRAM/CHAREN as outputs
            mach().poke(1, static_cast<Byte>((c64.peek(1) & ~7) | (v & 7)));
        }
        const Byte cfg = c64.config();
        return "$01=" + h2(c64.peek(1)) + " (LORAM=" + std::to_string(cfg & 1) + " HIRAM=" + std::to_string((cfg >> 1) & 1) +
               " CHAREN=" + std::to_string((cfg >> 2) & 1) + ")  " + c64.describeConfig() + "\n";
    }
    if (cmd == "io") {
        std::string out;
        for (const IoWrite& w : c64.takeIoLog()) {
            out += "  " + h4(w.address) + " <- " + h2(w.value) + "  " + pad(w.chip + (w.name.empty() ? "" : " " + w.name), 16) +
                   "by " + h4(w.pc) + "\n";
        }
        return out.empty() ? "no writes\n" : out;
    }
    if (cmd == "screen") {
        std::string out = "+" + std::string(40, '-') + "+  " + mach().screenMode() + ", screen at " + h4(c64.screenAddress()) + "\n";
        for (const std::string& l : mach().screenLines()) out += "|" + pad(l, 40) + "|\n";
        return out + "+" + std::string(40, '-') + "+\n";
    }
    Kernal& k = requireKernal(cmd);
    if (cmd == "kernal") {
        std::string out;
        for (const KernalCall& c : k.takeCalls()) {
            out += "  " + h4(c.address) + " " + pad(c.name.empty() ? "?" : c.name, 7) + " A=" + h2(c.before.a) + " X=" +
                   h2(c.before.x) + " Y=" + h2(c.before.y) + " -> A=" + h2(c.after.a) + " X=" + h2(c.after.x) + " Y=" +
                   h2(c.after.y) + " C=" + (c.after.flag(kFlagC) ? "1" : "0") + "  call at " + h4(c.callSite) +
                   (c.emulated ? "" : "  (NOT emulated)") + (c.note.empty() ? "" : "  [" + c.note + "]") + "\n";
        }
        return out.empty() ? "no KERNAL calls\n" : out;
    }
    if (cmd == "console") {
        const std::string text = k.takeConsole();
        return text.empty() ? "(empty)\n" : text + (text.back() == '\n' ? "" : "\n");
    }
    if (cmd == "keys") {
        if (a.size() < 2) throw MonitorError("usage: keys \"text\"");
        std::string text;
        for (std::size_t i = 1; i < a.size(); ++i) text += (i > 1 ? " " : "") + unquote(a[i]);
        std::string keys;
        for (std::size_t i = 0; i < text.size(); ++i) {  // \n in the text = RETURN
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
                keys += '\n';
                ++i;
            } else {
                keys += text[i];
            }
        }
        k.addKeys(keys);
        return "";
    }
    // run [adr]: like RUN with a BASIC starter, or SYS adr
    Address entry = 0;
    if (a.size() > 1) entry = address(a[1]);
    else if (auto t = findSysTarget(mach().memory(), 0x0801)) entry = *t;
    else throw MonitorError("no BASIC starter with SYS at $0801: give an address (run c000)");
    k.startProgram(entry);
    const std::uint64_t irqs = cpu().interruptCount();
    return afterRun(cpu().run(m.limit_), irqs);
}

std::string Monitor::Impl::assembleLine(const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())))) line.pop_back();
    const std::size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) {
        m.assembleAt_.reset();
        return "";
    }
    line = line.substr(b);
    const Address at = *m.assembleAt_;
    // "label:" prefix?
    std::string label;
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && colon > 0 && line.find_first_of(" \t;\"'") > colon) {
        label = line.substr(0, colon);
        line = line.substr(colon + 1);
        const std::size_t nb = line.find_first_not_of(" \t");
        line = nb == std::string::npos ? "" : line.substr(nb);
        std::string why;
        if (!isValidSymbolName(label, &why)) throw MonitorError("bad label '" + label + "': " + why);
    } else if (line.find_first_of(" \t=;") == std::string::npos && !isMnemonic(upper(line)) && line[0] != '.') {
        throw MonitorError("'" + line + "' is not an instruction (a label needs a colon: " + line + ":)");
    }
    if (line.empty() || line[0] == ';') {
        if (!label.empty()) mach().symbols[label] = at;
        return "";
    }
    AssemblerOptions opt;
    opt.origin = at;
    opt.predefined = mach().assemblerSymbols();
    opt.level = CpuLevel::All;
    if (!label.empty()) opt.predefined[label] = at;
    // "NAME = value" must start in column 1, instructions and directives must not
    const std::size_t sp = line.find_first_of(" \t=");
    const bool assignment = line.find('=') != std::string::npos && line[0] != '.' &&
                            !isMnemonic(upper(line.substr(0, sp)));
    const AssemblyResult r = assemble((assignment ? "" : "        ") + line + "\n", opt);
    if (!r.ok()) {
        std::string msg = r.errors[0].message;
        if (msg.find("undefined") != std::string::npos)
            msg += " (no forward references here: define the symbol first with 'sym name address')";
        throw MonitorError(msg);
    }
    if (!label.empty()) mach().symbols[label] = at;
    for (const auto& [k, v] : r.symbols)
        if (!opt.predefined.count(k)) mach().symbols[k] = v;  // NAME = value lines
    if (r.empty()) return "";
    const Address start = r.lowest();
    const std::vector<Byte> bytes = r.flatten();
    for (std::size_t i = 0; i < bytes.size(); ++i) mach().poke(static_cast<Address>(start + i), bytes[i]);
    m.assembleAt_ = static_cast<Address>(r.end() & 0xFFFF);
    m.nextDisassembly_ = start;
    return "  " + h4(start) + "  " + pad(bytesText(bytes), 10) + line + "\n";
}

std::string Monitor::Impl::cmdFiles(const std::string& cmd, const std::vector<std::string>& a) {
    requireFiles();
    if (a.size() < 2) throw MonitorError("usage: " + cmd + " file ...");
    const std::string file = unquote(a[1]);
    if (cmd == "symsave") {
        std::string text;
        for (const auto& [k, v] : mach().symbols) text += k + " = $" + h4(static_cast<unsigned>(v)) + "\n";
        writeTextFile(file, text);
        return "ok: " + std::to_string(mach().symbols.size()) + " symbols\n";
    }
    if (cmd == "symload") {
        std::map<Address, std::string> names;
        const std::vector<Diagnostic> errors = parseSymbolFile(readTextFile(file), names);
        if (!errors.empty()) throw MonitorError(file + ":" + std::to_string(errors[0].line) + ": " + errors[0].message);
        for (const auto& [addr, name] : names) mach().symbols[name] = addr;
        return "ok: " + std::to_string(names.size()) + " symbols\n";
    }
    if (cmd == "asm") {
        AssemblerOptions opt;
        if (a.size() > 2) opt.origin = address(a[2]);
        opt.predefined = mach().assemblerSymbols();
        const AssemblyResult r = assemble(readTextFile(file), opt);
        if (!r.ok()) {
            std::string out;
            for (const Diagnostic& d : r.errors) out += "? " + file + ":" + std::to_string(d.line) + ": " + d.message + "\n";
            return out;
        }
        std::size_t total = 0;
        for (const Segment& seg : r.segments) {
            mach().memory().load(seg.address, seg.bytes);
            total += seg.bytes.size();
        }
        std::size_t added = 0;
        for (const auto& [k, v] : r.symbols) {
            if (opt.predefined.count(k) && opt.predefined.at(k) == v) continue;
            mach().symbols[k] = v;
            ++added;
        }
        m.nextDisassembly_ = r.lowest();
        if (total == 0) return "ok: no bytes, " + std::to_string(added) + " symbols\n";
        return "ok: " + h4(r.lowest()) + "-" + h4(static_cast<unsigned>(r.end() - 1)) + " (" + std::to_string(total) + " bytes), " +
               std::to_string(added) + " symbols\n";
    }
    if (cmd == "l") {
        const std::optional<Address> at = a.size() > 2 ? std::optional<Address>(address(a[2])) : std::nullopt;
        const LoadedFile f = mach().loadFile(file, "auto", at);
        m.nextDisassembly_ = f.entry;
        m.nextDump_ = f.program.address;
        const Program& p = f.program;
        std::string out = "ok: " + h4(p.address) + "-" + h4(static_cast<unsigned>(p.bytes.empty() ? p.address : p.end() - 1)) + " (" +
                          std::to_string(p.bytes.size()) + " bytes), PC=" + h4(f.entry);
        if (!f.basic.empty()) out += "  (BASIC: " + f.basic + ")";
        if (!f.source.empty() && f.source != file) out += "  from " + f.source;
        return out + "\n";
    }
    if (cmd == "sv") {
        if (a.size() < 4) throw MonitorError("usage: sv file from to [raw]");
        const Address from = address(a[2]), to = address(a[3]);
        if (to < from) throw MonitorError("'to' is below 'from'");
        const bool raw = a.size() > 4 && lower(a[4]) == "raw";
        std::vector<Byte> bytes;
        for (std::uint32_t x = from; x <= to; ++x) bytes.push_back(mach().peek(static_cast<Address>(x)));
        writeBinaryFile(file, raw ? bytes : makePrg(from, bytes));
        return "ok: " + std::to_string(bytes.size()) + " bytes" + (raw ? " (raw)" : " (PRG, load address " + h4(from) + ")") + "\n";
    }
    if (cmd == "savestate") {
        writeBinaryFile(file, serialize(mach().capture()));
        return "ok\n";
    }
    if (cmd == "loadstate") {
        mach().restore(deserializeSnapshot(readBinaryFile(file)));
        m.nextDisassembly_ = regs().pc;
        return regsText() + currentLine();
    }
    // rom basic|kernal|char file
    requireC64("rom");
    if (a.size() < 3) throw MonitorError("usage: rom basic|kernal|char file");
    std::string which = lower(a[1]);
    if (which == "chargen") which = "char";
    mach().loadRom(which, readBinaryFile(unquote(a[2])));
    m.nextDisassembly_ = regs().pc;
    return "ok: " + which + " ROM loaded" + (which == "kernal" ? ", KERNAL emulation off, reset: PC=" + h4(regs().pc) : "") + "\n";
}

}  // namespace m6510
