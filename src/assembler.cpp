#include "m6510/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>

#include "m6510/encoder.hpp"
#include "m6510/expression.hpp"
#include "m6510/format.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

constexpr int kMaxPasses = 40;
constexpr std::uint32_t kMemoryEnd = 0x10000;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string>& directives() {
    static const std::set<std::string> d = {
        "ORG", ".ORG", "EQU", "=", ".BYTE", ".BYT", "DB", "DFB", ".TEXT", ".WORD", "DW", ".DBYTE", ".PETSCII", ".PET",
        ".SCREEN", ".SCR", ".NULL", ".LO", ".HI", ".LOHI", ".FILL", ".DS", "DS", ".RES", ".ALIGN", ".BASIC", ".CPU", "END",
        ".END"};
    return d;
}
bool isDirective(const std::string& upperName) { return directives().count(upperName) != 0; }

// Removes a trailing ';' comment (outside quotes and character constants).
std::string stripComment(const std::string& line) {
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"') {
            quote = c;
        } else if (c == '\'' && i + 2 < line.size() && line[i + 2] == '\'') {
            i += 2;
        } else if (c == ';') {
            return line.substr(0, i);
        }
    }
    return line;
}

struct SourceLine {
    int number = 0;
    std::string text;      // original text (for the listing)
    std::string label;     // "" if none
    std::string mnemonic;  // upper case, without .B/.W
    char force = 0;
    std::string operands;
    bool hasStatement = false;
    std::string parseError;
};

// Splits "LDA.W" into ("LDA", 'W'); leaves directives alone.
void splitForce(SourceLine& l, const std::string& token) {
    std::string mn = upper(token);
    const std::size_t dot = mn.rfind('.');
    if (dot != std::string::npos && dot > 0 && dot + 2 == mn.size() && (mn[dot + 1] == 'B' || mn[dot + 1] == 'W') &&
        isMnemonic(mn.substr(0, dot))) {
        l.force = mn[dot + 1];
        mn.resize(dot);
    }
    l.mnemonic = mn;
}

bool isStatementWord(const std::string& token) {
    const std::string u = upper(token);
    if (isDirective(u) || isMnemonic(u)) return true;
    const std::size_t dot = u.rfind('.');
    return dot != std::string::npos && dot > 0 && dot + 2 == u.size() && (u[dot + 1] == 'B' || u[dot + 1] == 'W') &&
           isMnemonic(u.substr(0, dot));
}

SourceLine parseLine(int number, const std::string& original) {
    SourceLine l;
    l.number = number;
    l.text = original;
    std::string s = stripComment(original);
    // "* = $C000" / "*=$C000" (also indented) sets the address; any other line with '*' in column 1 is a comment.
    if (const std::string t = trim(s); !t.empty() && t[0] == '*') {
        const std::string rest = trim(t.substr(1));
        if (!rest.empty() && rest[0] == '=') {
            l.mnemonic = "ORG";
            l.operands = trim(rest.substr(1));
            l.hasStatement = true;
            return l;
        }
        if (s[0] == '*') return l;
    }
    std::size_t i = 0;
    auto skipWs = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    };
    auto token = [&] {
        const std::size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '=') ++i;
        if (i == b && i < s.size() && s[i] == '=') ++i;  // a lone '='
        return s.substr(b, i - b);
    };
    if (trim(s).empty()) return l;

    if (!std::isspace(static_cast<unsigned char>(s[0]))) {
        const std::size_t save = i;
        std::string t = token();
        if (!t.empty() && t.back() == ':') {
            t.pop_back();
            l.label = t;
        } else if (isStatementWord(t)) {
            i = save;  // "NOP" in column 1 is an instruction, not a label
        } else {
            l.label = t;
        }
    }
    skipWs();
    if (i >= s.size()) return l;
    std::string mn = token();
    if (l.label.empty() && mn.size() > 1 && mn.back() == ':') {  // indented "label:"
        l.label = mn.substr(0, mn.size() - 1);
        skipWs();
        if (i >= s.size()) return l;
        mn = token();
    }
    skipWs();
    l.operands = trim(s.substr(i));
    // "* = expr" after a label is not allowed; "label = expr" / "label EQU expr" define a constant
    if (mn == "=") {
        l.mnemonic = "EQU";
    } else {
        splitForce(l, mn);
    }
    l.hasStatement = true;
    return l;
}

// Parses a string item "...". Returns false if `item` is no string.
bool stringItem(const std::string& item, std::string& out) {
    if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
        out = item.substr(1, item.size() - 2);
        return true;
    }
    return false;
}

struct Pass {
    Pass(const std::vector<SourceLine>& l, const AssemblerOptions& o, const std::map<std::string, std::int64_t>& p,
         std::vector<int>& h, std::vector<std::size_t>& len)
        : lines(l), options(o), previous(p), hints(h), lengths(len) {}

    const std::vector<SourceLine>& lines;
    const AssemblerOptions& options;
    const std::map<std::string, std::int64_t>& previous;
    std::vector<int>& hints;
    /// Length the statement of each line had when it last succeeded. A statement that fails in a pass keeps it, so that one
    /// spurious error (labels still a pass behind) cannot pull every following label forward and confirm itself.
    std::vector<std::size_t>& lengths;

    std::map<std::string, std::int64_t> symbols;
    std::vector<Segment> segments;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> undefinedUses;
    std::vector<ListingLine> listing;
    bool hintsChanged = false;
    std::uint32_t pc = 0;
    std::string lastGlobal;
    CpuLevel level = CpuLevel::Stable;

    void error(int line, const std::string& m) { errors.push_back({line, m}); }

    std::string scoped(const std::string& name) const { return !name.empty() && name[0] == '.' ? lastGlobal + name : name; }

    bool lookup(const std::string& raw, std::int64_t& v) const {
        const std::string name = scoped(raw);
        for (const auto* table : {&symbols, &previous, &options.predefined}) {
            if (auto it = table->find(name); it != table->end()) {
                v = it->second;
                return true;
            }
        }
        return false;
    }

    // Evaluates an expression. Returns false (and reports) on a syntax error.
    bool eval(int line, const std::string& text, std::int64_t& out, bool& undefined) {
        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& v) { return lookup(n, v); }, pc);
        const ExprResult r = ev.evaluate(text);
        if (!r.ok) {
            error(line, r.error);
            return false;
        }
        out = r.value;
        undefined = r.undefined;
        if (r.undefined) undefinedUses.push_back({line, r.undefinedName});
        return true;
    }

    void emit(const std::vector<Byte>& bytes, int line) {
        if (bytes.empty()) return;
        if (pc + bytes.size() > kMemoryEnd) {
            error(line, "code or data beyond $FFFF");
            pc += static_cast<std::uint32_t>(bytes.size());
            return;
        }
        if (segments.empty() || segments.back().address + segments.back().bytes.size() != pc) {
            segments.push_back({static_cast<Address>(pc), {}});
        }
        segments.back().bytes.insert(segments.back().bytes.end(), bytes.begin(), bytes.end());
        pc += static_cast<std::uint32_t>(bytes.size());
    }

    void define(int line, const std::string& rawName, std::int64_t value) {
        std::string why;
        if (!isValidSymbolName(rawName, &why)) {
            error(line, why);
            return;
        }
        const std::string name = scoped(rawName);
        if (rawName[0] == '.' && lastGlobal.empty()) {
            error(line, "local label '" + rawName + "' before the first global label");
            return;
        }
        if (symbols.count(name) || options.predefined.count(name)) {
            error(line, "symbol '" + rawName + "' defined twice");
            return;
        }
        symbols[name] = value;
    }

    // ---- data directives -----------------------------------------------------------------------
    // kind: 'B' bytes, 'W' words, 'D' big-endian words, 'P' petscii, 'S' screen codes, 'L' low bytes, 'H' high bytes
    void dataItems(const SourceLine& l, char kind, std::vector<Byte>& out) {
        const std::vector<std::string> items = splitOperands(l.operands);
        if (items.empty()) {
            error(l.number, l.mnemonic + " needs at least one value");
            return;
        }
        for (const std::string& item : items) {
            if (item.empty()) {
                error(l.number, "empty item in " + l.mnemonic);
                continue;
            }
            std::string str;
            if (stringItem(item, str)) {
                if (kind == 'W' || kind == 'D' || kind == 'L' || kind == 'H') {
                    error(l.number, "a string is not allowed in " + l.mnemonic);
                    continue;
                }
                for (char c : str) {
                    if (kind == 'P' || kind == 'S') {
                        const auto b = kind == 'P' ? asciiToPetscii(c) : asciiToScreenCode(c);
                        if (!b) {
                            error(l.number, std::string("character '") + c + "' has no " +
                                                (kind == 'P' ? "PETSCII" : "screen code") + " equivalent");
                            continue;
                        }
                        out.push_back(*b);
                    } else {
                        out.push_back(static_cast<Byte>(c));
                    }
                }
                continue;
            }
            std::int64_t v = 0;
            bool undef = false;
            if (!eval(l.number, item, v, undef)) continue;
            if (kind == 'W' || kind == 'D') {
                if (!undef && (v < -32768 || v > 0xFFFF)) error(l.number, "value " + std::to_string(v) + " out of range for a word");
                const auto lo = static_cast<Byte>(v & 0xFF), hi = static_cast<Byte>((v >> 8) & 0xFF);
                if (kind == 'W') out.insert(out.end(), {lo, hi});
                else out.insert(out.end(), {hi, lo});
            } else if (kind == 'L' || kind == 'H') {
                if (!undef && (v < 0 || v > 0xFFFF)) error(l.number, "value " + std::to_string(v) + " out of range for an address");
                out.push_back(static_cast<Byte>(kind == 'L' ? (v & 0xFF) : ((v >> 8) & 0xFF)));
            } else {
                if (!undef && (v < -128 || v > 255)) error(l.number, "value " + std::to_string(v) + " out of range for a byte");
                out.push_back(static_cast<Byte>(v & 0xFF));
            }
        }
    }

    bool evalDefined(const SourceLine& l, const std::string& text, std::int64_t& v, const char* what) {
        bool undef = false;
        if (!eval(l.number, text, v, undef)) return false;
        if (undef) {
            undefinedUses.pop_back();
            error(l.number, std::string(what) + " needs a value that is defined before this line");
            return false;
        }
        return true;
    }

    void fill(const SourceLine& l, std::vector<Byte>& out) {
        const std::vector<std::string> ops = splitOperands(l.operands);
        if (ops.empty() || ops.size() > 2) return error(l.number, l.mnemonic + " needs a count and optionally a fill value");
        std::int64_t n = 0, value = 0;
        if (!evalDefined(l, ops[0], n, l.mnemonic.c_str())) return;
        if (n < 0 || n > 0x10000) return error(l.number, "bad count for " + l.mnemonic);
        if (ops.size() == 2) {
            bool undef = false;
            if (!eval(l.number, ops[1], value, undef)) return;
            if (!undef && (value < -128 || value > 255)) return error(l.number, "fill value out of range for a byte");
        }
        out.assign(static_cast<std::size_t>(n), static_cast<Byte>(value & 0xFF));
    }

    void align(const SourceLine& l, std::vector<Byte>& out) {
        const std::vector<std::string> ops = splitOperands(l.operands);
        if (ops.empty() || ops.size() > 2) return error(l.number, ".ALIGN needs a size and optionally a fill value");
        std::int64_t n = 0, value = 0;
        if (!evalDefined(l, ops[0], n, ".ALIGN")) return;
        if (n < 1 || n > 0x10000) return error(l.number, "bad .ALIGN size");
        if (ops.size() == 2) {
            bool undef = false;
            if (!eval(l.number, ops[1], value, undef)) return;
        }
        const std::uint32_t pad = static_cast<std::uint32_t>((n - static_cast<std::int64_t>(pc % static_cast<std::uint32_t>(n))) % n);
        out.assign(pad, static_cast<Byte>(value & 0xFF));
    }

    // .BASIC [line][,target]
    void basic(const SourceLine& l, std::size_t idx, std::vector<Byte>& out) {
        const std::vector<std::string> ops = l.operands.empty() ? std::vector<std::string>{} : splitOperands(l.operands);
        if (ops.size() > 2) return error(l.number, ".BASIC takes a line number and a target address");
        std::int64_t line = 10;
        if (!ops.empty() && !ops[0].empty()) {
            bool undef = false;
            if (!eval(l.number, ops[0], line, undef)) return;
            if (undef) line = 10;
            if (line < 0 || line > 63999) return error(l.number, "BASIC line numbers are 0..63999");
        }
        std::int64_t target = 0;
        if (ops.size() == 2) {
            bool undef = false;
            if (!eval(l.number, ops[1], target, undef)) return;
            if (undef) target = pc + (lengths[idx] ? lengths[idx] : 12);  // placeholder until it is known
            if (target < 0 || target > 0xFFFF) return error(l.number, ".BASIC target out of range");
        } else {
            // directly behind the starter: the length depends on the number of digits of the target
            for (int digits = 1; digits <= 5; ++digits) {
                const std::int64_t t = static_cast<std::int64_t>(pc) + 8 + digits;
                if (static_cast<int>(std::to_string(t).size()) == digits) {
                    target = t;
                    break;
                }
            }
        }
        out = makeBasicStub(static_cast<Address>(pc), static_cast<unsigned>(line), static_cast<unsigned>(target));
    }

    void cpu(const SourceLine& l) {
        const std::string c = upper(trim(l.operands));
        if (c == "6502") level = CpuLevel::Documented;
        else if (c == "6510") level = CpuLevel::Stable;
        else if (c == "6510X") level = CpuLevel::All;
        else error(l.number, "unknown CPU '" + l.operands + "' (6502, 6510 or 6510X)");
    }

    void run() {
        pc = options.origin;
        level = options.level;
        for (std::size_t idx = 0; idx < lines.size(); ++idx) {
            const SourceLine& l = lines[idx];
            const std::uint32_t startPc = pc;
            std::vector<Byte> produced;
            bool stop = false;
            if (!l.parseError.empty()) error(l.number, l.parseError);

            const bool isEqu = l.hasStatement && l.mnemonic == "EQU";
            if (!l.label.empty() && !isEqu) {
                if (l.label[0] != '.') lastGlobal = l.label;
                define(l.number, l.label, pc);
            }
            const std::size_t errorsBefore = errors.size();
            if (l.hasStatement) {
                const std::string& m = l.mnemonic;
                if (m == "EQU") {
                    if (l.label.empty()) {
                        error(l.number, "EQU / '=' needs a label");
                    } else {
                        std::int64_t v = 0;
                        bool undef = false;
                        if (eval(l.number, l.operands, v, undef) && !undef) {
                            if (l.label[0] != '.') lastGlobal = l.label;
                            define(l.number, l.label, v);
                        }
                    }
                } else if (m == "ORG" || m == ".ORG") {
                    std::int64_t v = 0;
                    if (evalDefined(l, l.operands, v, "ORG")) {
                        if (v < 0 || v > 0xFFFF) error(l.number, "ORG address out of range (0..$FFFF)");
                        else pc = static_cast<std::uint32_t>(v);
                    }
                } else if (m == "END" || m == ".END") {
                    stop = true;
                } else if (m == ".BYTE" || m == ".BYT" || m == "DB" || m == "DFB" || m == ".TEXT") {
                    dataItems(l, 'B', produced);
                } else if (m == ".NULL") {
                    dataItems(l, 'B', produced);
                    produced.push_back(0);
                } else if (m == ".WORD" || m == "DW") {
                    dataItems(l, 'W', produced);
                } else if (m == ".DBYTE") {
                    dataItems(l, 'D', produced);
                } else if (m == ".PETSCII" || m == ".PET") {
                    dataItems(l, 'P', produced);
                } else if (m == ".SCREEN" || m == ".SCR") {
                    dataItems(l, 'S', produced);
                } else if (m == ".LO") {
                    dataItems(l, 'L', produced);
                } else if (m == ".HI") {
                    dataItems(l, 'H', produced);
                } else if (m == ".LOHI") {
                    std::vector<Byte> hi;
                    dataItems(l, 'L', produced);
                    dataItems(l, 'H', hi);
                    produced.insert(produced.end(), hi.begin(), hi.end());
                } else if (m == ".FILL" || m == ".DS" || m == "DS" || m == ".RES") {
                    fill(l, produced);
                } else if (m == ".ALIGN") {
                    align(l, produced);
                } else if (m == ".BASIC") {
                    basic(l, idx, produced);
                } else if (m == ".CPU") {
                    cpu(l);
                } else if (isMnemonic(m)) {
                    EncodeRequest rq;
                    rq.mnemonic = m;
                    rq.force = l.force;
                    rq.operand = l.operands;
                    rq.pc = static_cast<Address>(pc);
                    rq.level = level;
                    rq.hint = hints[idx];
                    const EncodeResult r = encodeInstruction(rq, [&](const std::string& e) {
                        EncodeValue v;
                        std::int64_t value = 0;
                        bool undef = false;
                        const std::size_t before = errors.size();
                        if (!eval(l.number, e, value, undef)) {
                            v.error = errors.back().message;
                            errors.resize(before);
                            return v;
                        }
                        v.value = value;
                        v.undefined = undef;
                        return v;
                    });
                    if (!r.ok) {
                        error(l.number, r.error);
                    } else {
                        if (r.level != hints[idx]) {
                            hints[idx] = r.level;
                            hintsChanged = true;
                        }
                        produced = r.bytes;
                    }
                } else {
                    error(l.number, "unknown instruction or directive '" + l.mnemonic + "'");
                }
            }
            if (errors.size() != errorsBefore && produced.empty() && lengths[idx] > 0 && l.hasStatement &&
                (isMnemonic(l.mnemonic) || l.mnemonic == ".BASIC")) {
                produced.assign(lengths[idx], 0);  // keeps the labels where they were; the bytes go with the errors
            } else if (errors.size() == errorsBefore && !produced.empty()) {
                lengths[idx] = produced.size();
            }
            emit(produced, l.number);
            listing.push_back({l.number, static_cast<Address>(startPc & 0xFFFF), produced, l.text});
            if (stop) break;
        }
    }
};

}  // namespace

bool isValidSymbolName(const std::string& name, std::string* why) {
    auto reject = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    if (name.empty()) return reject("empty symbol name");
    const std::size_t start = name[0] == '.' ? 1 : 0;
    if (start >= name.size() || !(std::isalpha(static_cast<unsigned char>(name[start])) || name[start] == '_')) {
        return reject("bad symbol name '" + name + "' (must start with a letter or '_')");
    }
    for (std::size_t i = start; i < name.size(); ++i) {
        if (!(std::isalnum(static_cast<unsigned char>(name[i])) || name[i] == '_')) {
            return reject("bad character '" + std::string(1, name[i]) + "' in symbol name '" + name + "'");
        }
    }
    const std::string u = upper(name.substr(start));
    if (u == "A" || u == "X" || u == "Y") return reject("'" + name + "' is a register name");
    if (start == 0 && (isMnemonic(u) || isDirective(u))) return reject("'" + name + "' is an instruction or directive name");
    if (u == "LOW" || u == "HIGH") return reject("'" + name + "' is a function name");
    return true;
}

bool AssemblyResult::empty() const {
    for (const Segment& s : segments)
        if (!s.bytes.empty()) return false;
    return true;
}

Address AssemblyResult::lowest() const {
    std::uint32_t lo = kMemoryEnd;
    for (const Segment& s : segments)
        if (!s.bytes.empty()) lo = std::min<std::uint32_t>(lo, s.address);
    return lo == kMemoryEnd ? 0 : static_cast<Address>(lo);
}

std::uint32_t AssemblyResult::end() const {
    std::uint32_t hi = 0;
    for (const Segment& s : segments)
        if (!s.bytes.empty()) hi = std::max<std::uint32_t>(hi, s.address + static_cast<std::uint32_t>(s.bytes.size()));
    return hi;
}

std::vector<Byte> AssemblyResult::flatten() const {
    std::vector<Byte> out;
    const Address lo = lowest();
    for (const Segment& s : segments) {
        if (s.bytes.empty()) continue;
        const std::size_t off = s.address - lo;
        if (out.size() < off + s.bytes.size()) out.resize(off + s.bytes.size(), 0);
        std::copy(s.bytes.begin(), s.bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(off));
    }
    return out;
}

std::string AssemblyResult::formatListing() const {
    std::ostringstream out;
    for (const ListingLine& l : listing) {
        std::string bytes;
        for (std::size_t i = 0; i < l.bytes.size() && i < 4; ++i) {
            if (i) bytes += ' ';
            bytes += hexDigits(l.bytes[i], 2);
        }
        if (l.bytes.size() > 4) bytes += "..";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%5d  %04X  %-13s ", l.line, static_cast<unsigned>(l.address), bytes.c_str());
        out << buf << l.source << '\n';
    }
    return out.str();
}

std::string AssemblyResult::formatSymbols() const {
    std::ostringstream out;
    for (const auto& [name, value] : symbols) {
        out << name << " = " << formatHex(static_cast<unsigned>(value & 0xFFFF), 4, NumberStyle::Dollar) << '\n';
    }
    return out.str();
}

std::string AssemblyResult::formatViceLabels() const {
    std::ostringstream out;
    for (const auto& [name, value] : symbols) {
        if (value < 0 || value > 0xFFFF) continue;
        std::string n = name;
        std::replace(n.begin(), n.end(), '.', '_');
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04x", static_cast<unsigned>(value));
        out << "al C:" << buf << " ." << n << '\n';
    }
    return out.str();
}

AssemblyResult assemble(const std::string& source, const AssemblerOptions& options) {
    std::vector<SourceLine> lines;
    {
        std::istringstream in(source);
        std::string text;
        int n = 0;
        while (std::getline(in, text)) {
            if (!text.empty() && text.back() == '\r') text.pop_back();
            lines.push_back(parseLine(++n, text));
        }
    }

    std::vector<int> hints(lines.size(), 0);
    std::vector<std::size_t> lengths(lines.size(), 0);
    std::map<std::string, std::int64_t> previous;
    AssemblyResult result;
    for (int passNo = 0; passNo < kMaxPasses; ++passNo) {
        Pass pass(lines, options, previous, hints, lengths);
        pass.run();
        const bool stable = !pass.hintsChanged && pass.symbols == previous;
        if (stable || passNo == kMaxPasses - 1) {
            result.segments = std::move(pass.segments);
            result.errors = std::move(pass.errors);
            result.listing = std::move(pass.listing);
            result.symbols = std::move(pass.symbols);
            std::set<std::pair<int, std::string>> reported;
            for (const Diagnostic& u : pass.undefinedUses) {
                if (reported.insert({u.line, u.message}).second) {
                    result.errors.push_back({u.line, "undefined symbol '" + u.message + "'"});
                }
            }
            if (!stable) result.errors.push_back({0, "assembly did not converge (symbol values keep changing)"});
            std::stable_sort(result.errors.begin(), result.errors.end(),
                             [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
            if (!result.errors.empty()) {
                // the bytes of a failed run are not trustworthy
            }
            break;
        }
        previous = pass.symbols;
    }
    return result;
}

}  // namespace m6510
