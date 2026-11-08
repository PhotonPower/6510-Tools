#include "m6510/disassembler.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "m6510/expression.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

bool printable(Byte b) { return b >= 0x20 && b <= 0x7E && b != '"'; }

// ".BYTE" operand list with printable runs (at least 3 characters) as strings.
std::string byteList(const std::vector<Byte>& bytes, NumberStyle style) {
    std::string out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::size_t j = i;
        while (j < bytes.size() && printable(bytes[j])) ++j;
        if (!out.empty()) out += ',';
        if (j - i >= 3) {
            out += '"';
            for (std::size_t k = i; k < j; ++k) out += static_cast<char>(bytes[k]);
            out += '"';
            i = j;
        } else {
            out += formatHex(bytes[i], 2, style);
            ++i;
        }
    }
    return out;
}

std::string asciiColumn(const std::vector<Byte>& bytes) {
    std::string s = "|";
    for (Byte b : bytes) s += (b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '.';
    return s + "|";
}

}  // namespace

Disassembler::Disassembler(const Memory& memory, Address start, std::uint32_t end, CpuLevel level)
    : memory_(memory), start_(start), end_(std::min<std::uint32_t>(std::max<std::uint32_t>(end, start), 0x10000)),
      level_(level), state_(end_ - start_, kData) {}

void Disassembler::addEntry(Address address) { entries_.insert(address); }
void Disassembler::addSymbol(Address address, const std::string& name) { symbols_[address] = name; }
void Disassembler::addComment(Address address, const std::string& text) {
    auto& c = comments_[address];
    c = c.empty() ? text : c + "; " + text;
}

Instruction Disassembler::decodeAt(Address a, NumberStyle style) const {
    DecodeOptions o;
    o.level = level_;
    o.style = style;
    return decode(memory_, a, o);
}

void Disassembler::detectBasic() {
    if (const auto stub = parseSysStub(memory_, static_cast<Address>(start_));
        stub && start_ + stub->length <= end_) {
        basicLength_ = stub->length;
        basicLine_ = stub->line;
        basicTarget_ = stub->target;
        for (std::size_t i = 0; i < basicLength_; ++i) state_[i] = kBasic;
        if (inRange(basicTarget_)) labels_.insert(basicTarget_);
        if (entries_.empty()) entries_.insert(basicTarget_);
    } else if (entries_.empty()) {
        if (const auto t = findSysTarget(memory_, static_cast<Address>(start_)); t && inRange(*t)) entries_.insert(*t);
    }
}

bool Disassembler::mark(const Instruction& in) {
    const std::uint32_t a = in.address;
    if (!inRange(a) || a + in.length() > end_) return false;
    for (std::size_t i = 0; i < in.length(); ++i)
        if (state(a + static_cast<std::uint32_t>(i)) != kData) return false;
    state_[a - start_] = kStart;
    for (std::size_t i = 1; i < in.length(); ++i) state_[a - start_ + i] = kCont;
    return true;
}

void Disassembler::noteReferences(const Instruction& in, std::vector<Address>* work) {
    if (in.target) {
        xrefs_[*in.target].insert(in.address);
        if (inRange(*in.target)) {
            labels_.insert(*in.target);
            if (work) work->push_back(*in.target);
        }
    }
    if (in.reference && inRange(*in.reference)) {
        labels_.insert(*in.reference);
        xrefs_[*in.reference].insert(in.address);
    }
}

void Disassembler::analyzeFlow() {
    detectBasic();
    if (entries_.empty()) entries_.insert(static_cast<Address>(start_));
    std::vector<Address> work(entries_.begin(), entries_.end());
    for (Address e : entries_)
        if (inRange(e)) labels_.insert(e);
    while (!work.empty()) {
        Address a = work.back();
        work.pop_back();
        while (inRange(a) && state(a) == kData) {
            const Instruction in = decodeAt(a, NumberStyle::Dollar);
            if (!in.valid || !mark(in)) break;
            noteReferences(in, &work);
            if (in.flow == FlowKind::Jump || in.flow == FlowKind::Return || in.flow == FlowKind::Halt) break;
            a = static_cast<Address>(a + in.length());
            if (a == 0) break;  // wrapped at $FFFF
        }
    }
    finishLabels();
}

void Disassembler::analyzeLinear() {
    detectBasic();
    std::uint32_t a = start_ + static_cast<std::uint32_t>(basicLength_);
    while (a < end_) {
        const Instruction in = decodeAt(static_cast<Address>(a), NumberStyle::Dollar);
        if (in.valid && mark(in)) {
            noteReferences(in, nullptr);
            a += static_cast<std::uint32_t>(in.length());
        } else {
            ++a;
        }
    }
    finishLabels();
}

// Labels in the middle of an instruction are written as `start+n`: give the instruction a label instead.
void Disassembler::finishLabels() {
    std::set<Address> extra;
    for (Address a : labels_)
        if (inRange(a) && state(a) == kCont) extra.insert(lineStart(a));
    labels_.insert(extra.begin(), extra.end());
}

std::size_t Disassembler::codeBytes() const {
    return static_cast<std::size_t>(std::count_if(state_.begin(), state_.end(), [](std::uint8_t s) { return s == kStart || s == kCont; }));
}

Address Disassembler::lineStart(Address a) const {
    std::uint32_t s = a;
    while (s > start_ && state(s) == kCont) --s;
    return static_cast<Address>(s);
}

std::string Disassembler::labelName(Address a) const {
    if (auto it = symbols_.find(a); it != symbols_.end()) return it->second;
    return "L_" + hexDigits(a, 4);
}

std::string Disassembler::symbolFor(Address a) const {
    if (inRange(a)) {
        if (state(a) == kCont) {
            const Address s = lineStart(a);
            return labelName(s) + "+" + std::to_string(a - s);
        }
        if (state(a) == kBasic && a != start_) return "";  // inside the BASIC starter: no line to put a label on
        if (labels_.count(a)) return labelName(a);
        return "";
    }
    if (auto it = symbols_.find(a); it != symbols_.end()) return it->second;
    return "";
}

std::string Disassembler::render(const DisassemblerOptions& options) const {
    std::ostringstream out;
    const NumberStyle st = options.style;
    const bool src = options.asmSource;

    DecodeOptions dopt;
    dopt.level = level_;
    dopt.style = st;
    dopt.symbolizer = [this](Address a) { return symbolFor(a); };

    // Pre-scan the instructions to find the external symbols that are used (for `name = value` lines).
    std::set<Address> usedExternal;
    for (std::uint32_t a = start_; a < end_; ++a) {
        if (state(a) != kStart) continue;
        const Instruction in = decode(memory_, static_cast<Address>(a), dopt);
        for (const auto& opt : {in.target, in.reference})
            if (opt && !inRange(*opt) && symbols_.count(*opt)) usedExternal.insert(*opt);
    }

    out << "; 6510 disassembly  " << formatHex(start_, 4, st) << " - " << formatHex(end_ - 1, 4, st) << "\n";
    if (!usedExternal.empty()) {
        out << "\n";
        std::size_t width = 0;
        for (Address a : usedExternal) width = std::max(width, symbols_.at(a).size());
        for (Address a : usedExternal) out << pad(symbols_.at(a), width) << " = " << formatHex(a, 4, st) << "\n";
    }
    out << "\n";
    if (src) out << "        * = " << formatHex(start_, 4, st) << "\n";

    auto labelLine = [&](Address a) {
        std::string l = labelName(a) + ":";
        if (options.xrefs && !src) {
            if (auto it = xrefs_.find(a); it != xrefs_.end() && !it->second.empty()) {
                std::string refs;
                std::size_t n = 0;
                for (Address from : it->second) {
                    if (n++ == 4) {
                        refs += " ...";
                        break;
                    }
                    refs += (refs.empty() ? "" : " ") + formatHex(from, 4, st);
                }
                l = pad(l, 36) + "; <- " + refs;
            }
        }
        out << l << "\n";
    };
    auto codeLine = [&](Address a, const std::vector<Byte>& bytes, const std::string& text, const std::string& comment) {
        std::string line;
        if (src) {
            line = "        " + text;
        } else {
            line = hexDigits(a, 4) + "  ";
            if (options.showBytes) {
                std::string b;
                for (std::size_t i = 0; i < bytes.size() && i < 3; ++i) b += hexDigits(bytes[i], 2) + " ";
                line += pad(b, 10);
            } else {
                line += "   ";
            }
            line += text;
        }
        if (!comment.empty()) line = pad(line, src ? 40 : 46) + "; " + comment;
        out << line << "\n";
    };
    auto commentAt = [&](Address a) -> std::string {
        auto it = comments_.find(a);
        return it == comments_.end() ? "" : it->second;
    };

    std::uint32_t a = start_;
    if (basicLength_ > 0) {
        if (labels_.count(static_cast<Address>(start_))) labelLine(static_cast<Address>(start_));
        const std::string target = symbolFor(basicTarget_);
        std::string c = std::to_string(basicLine_) + " SYS" + std::to_string(basicTarget_);
        if (!commentAt(static_cast<Address>(a)).empty()) c += "; " + commentAt(static_cast<Address>(a));
        codeLine(static_cast<Address>(a), memory_.readBytes(static_cast<Address>(a), 3),
                 ".BASIC " + std::to_string(basicLine_) + "," + (target.empty() ? formatHex(basicTarget_, 4, st) : target), c);
        a += static_cast<std::uint32_t>(basicLength_);
    }
    while (a < end_) {
        const auto addr = static_cast<Address>(a);
        if (labels_.count(addr) && state(a) != kCont) labelLine(addr);
        if (state(a) == kStart) {
            const Instruction in = decode(memory_, addr, dopt);
            std::string comment = commentAt(addr);
            if (src && !in.canonical) {
                // the assembler would choose another opcode for this text: keep the bytes
                comment = in.text() + (comment.empty() ? "" : "; " + comment);
                codeLine(addr, in.bytes, ".BYTE " + byteList(in.bytes, NumberStyle::Dollar), comment);
            } else {
                codeLine(addr, in.bytes, in.text(), comment);
            }
            a += static_cast<std::uint32_t>(in.length());
            continue;
        }
        // data: up to 8 bytes, a new line at every label and comment
        std::vector<Byte> bytes;
        std::uint32_t b = a;
        while (b < end_ && bytes.size() < 8 && state(b) == kData &&
               (b == a || (!labels_.count(static_cast<Address>(b)) && !comments_.count(static_cast<Address>(b))))) {
            bytes.push_back(memory_.read8(static_cast<Address>(b)));
            ++b;
        }
        if (bytes.empty()) {  // kCont without a start (cannot happen) - never loop forever
            bytes.push_back(memory_.read8(addr));
            b = a + 1;
        }
        std::string comment = commentAt(addr);
        if (!src) comment = asciiColumn(bytes) + (comment.empty() ? "" : " " + comment);
        codeLine(addr, bytes, ".BYTE " + byteList(bytes, st), comment);
        a = b;
    }
    return out.str();
}

std::vector<Diagnostic> loadCommentFile(const std::string& text, Disassembler& dis) {
    std::vector<Diagnostic> diags;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';') continue;
        const std::size_t comma = t.find(',');
        if (comma == std::string::npos) {
            diags.push_back({n, "expected address,\"text\""});
            continue;
        }
        const ExprResult r = ExpressionEvaluator(nullptr, 0).evaluate(trim(t.substr(0, comma)));
        if (!r.ok || r.undefined || r.value < 0 || r.value > 0xFFFF) {
            diags.push_back({n, "bad address '" + trim(t.substr(0, comma)) + "'"});
            continue;
        }
        std::string c = trim(t.substr(comma + 1));
        if (c.size() >= 2 && c.front() == '"' && c.back() == '"') c = c.substr(1, c.size() - 2);
        const auto a = static_cast<Address>(r.value);
        dis.addEntry(a);
        dis.addComment(a, c);
    }
    return diags;
}

std::vector<Diagnostic> parseSymbolFile(const std::string& text, std::map<Address, std::string>& out) {
    std::vector<Diagnostic> diags;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        std::string name, value;
        if (t.rfind("al ", 0) == 0) {  // VICE: al C:080d .start
            std::istringstream ls(t.substr(3));
            std::string addr;
            ls >> addr >> name;
            if (addr.size() > 2 && addr[1] == ':') addr = addr.substr(2);
            value = "$" + addr;
            if (!name.empty() && name[0] == '.') name = name.substr(1);
        } else if (const std::size_t eq = t.find('='); eq != std::string::npos) {
            name = trim(t.substr(0, eq));
            value = trim(t.substr(eq + 1));
        }
        const ExprResult r = ExpressionEvaluator(nullptr, 0).evaluate(value);
        if (name.empty() || !r.ok || r.undefined || r.value < 0 || r.value > 0xFFFF) {
            diags.push_back({n, "expected 'name = $XXXX' or 'al C:XXXX .name'"});
            continue;
        }
        out[static_cast<Address>(r.value)] = name;
    }
    return diags;
}

const std::map<Address, std::string>& kernalSymbols() {
    static const std::map<Address, std::string> k = {
        {0xFF81, "CINT"},   {0xFF84, "IOINIT"}, {0xFF87, "RAMTAS"}, {0xFF8A, "RESTOR"}, {0xFF8D, "VECTOR"},
        {0xFF90, "SETMSG"}, {0xFF93, "SECOND"}, {0xFF96, "TKSA"},   {0xFF99, "MEMTOP"}, {0xFF9C, "MEMBOT"},
        {0xFF9F, "SCNKEY"}, {0xFFA2, "SETTMO"}, {0xFFA5, "ACPTR"},  {0xFFA8, "CIOUT"},  {0xFFAB, "UNTLK"},
        {0xFFAE, "UNLSN"},  {0xFFB1, "LISTEN"}, {0xFFB4, "TALK"},   {0xFFB7, "READST"}, {0xFFBA, "SETLFS"},
        {0xFFBD, "SETNAM"}, {0xFFC0, "OPEN"},   {0xFFC3, "CLOSE"},  {0xFFC6, "CHKIN"},  {0xFFC9, "CHKOUT"},
        {0xFFCC, "CLRCHN"}, {0xFFCF, "CHRIN"},  {0xFFD2, "CHROUT"}, {0xFFD5, "LOAD"},   {0xFFD8, "SAVE"},
        {0xFFDB, "SETTIM"}, {0xFFDE, "RDTIM"},  {0xFFE1, "STOP"},   {0xFFE4, "GETIN"},  {0xFFE7, "CLALL"},
        {0xFFEA, "UDTIM"},  {0xFFED, "SCREEN"}, {0xFFF0, "PLOT"},   {0xFFF3, "IOBASE"},
    };
    return k;
}

}  // namespace m6510
