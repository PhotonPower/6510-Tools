#include "m6510/listing.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "m6510/assembler.hpp"
#include "m6510/c64.hpp"
#include "m6510/decoder.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/format.hpp"
#include "m6510/petscii.hpp"
#include "m6510/version.hpp"

namespace m6510 {

namespace {

std::string h2(unsigned v) { return "$" + hexDigits(v & 0xFF, 2); }
std::string h4(unsigned v) { return "$" + hexDigits(v & 0xFFFF, 4); }

std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    else s += ' ';
    return s;
}

// Chip registers are mirrored; only the base address gets the register's name (a mirror stays a number).
bool isBaseRegister(Address a) {
    if (a >= 0xD000 && a < 0xD400) return a < 0xD040;
    if (a >= 0xD400 && a < 0xD800) return a < 0xD420;
    if (a >= 0xDC00 && a < 0xDE00) return (a & 0xFF) < 0x10;
    return false;
}

// Characters that can stand inside a quoted string of the listing without confusing the parser.
bool stringSafe(char c) { return c >= 0x20 && c < 0x7F && c != '"' && c != ';' && c != ',' && c != '\\'; }

struct Item {
    enum Kind { Code, Region, Table, Bytes, Fill, Basic } kind = Bytes;
    Address start = 0;
    unsigned length = 0;
    const DataRegion* region = nullptr;
    bool hiSide = false;  // Lohi region / split table: this item is the table of high bytes
    const JumpTable* table = nullptr;
    SysStub stub{};
};

class Renderer {
public:
    Renderer(const Memory& view, const Analysis& an, const Annotations& ann, const ListingOptions& o)
        : view_(view), an_(an), ann_(ann), o_(o) {}

    ListingResult run();

private:
    const Memory& view_;
    const Analysis& an_;
    const Annotations& ann_;
    const ListingOptions& o_;
    std::map<Address, Item> items_;
    std::set<Address> needLabel_, labelUsed_, routineEntries_, scalarRefs_;
    std::map<Address, std::string> equates_;  // address -> name used outside the range
    std::map<std::string, Address> equateNames_;

    [[nodiscard]] bool inList(std::uint32_t a) const { return a >= o_.from && a < o_.to; }
    [[nodiscard]] std::string nameFor(Address a) const;
    std::string labelExpr(Address a);
    std::string symExpr(Address a);
    std::string systemName(Address a);
    std::optional<Address> namedRegionInside(Address a) const;
    void useEquate(const std::string& name, Address a);
    void collectReferences();
    void buildItems();
    std::vector<std::string> renderItem(const Item& it, unsigned& userComments, unsigned& instructions);
    std::vector<std::string> renderCode(const Item& it, unsigned& userComments);
    std::string tableEntry(Address target, bool minusOne, Address raw);
    /// "$0860: first / second" (the address only with the `addresses` option).
    [[nodiscard]] std::string commentText(Address a, const std::vector<std::string>& parts) const {
        std::string c;
        for (const std::string& x : parts) c += (c.empty() ? "" : " / ") + x;
        if (o_.addresses) c = h4(a) + (c.empty() ? "" : ": " + c);
        return c;
    }
};

std::string Renderer::nameFor(Address a) const {
    if (const auto n = ann_.nameAt(a)) return *n;
    if (routineEntries_.count(a)) return "sub_" + hexDigits(a, 4);
    if (an_.at(a)) return "loc_" + hexDigits(a, 4);
    return "data_" + hexDigits(a, 4);
}

std::optional<Address> Renderer::namedRegionInside(Address a) const {
    // an address inside (not at the start of) a named data region or a named variable of several bytes, without a name of
    // its own: it is written as name+offset and gets no label (no data_XXXX in the middle of a named table)
    if (ann_.nameAt(a) || ann_.variableAt(a)) return std::nullopt;
    for (const VariableDoc& v : ann_.variables())
        if (a > v.address && a < v.address + v.size && inList(v.address)) return v.address;
    const DataRegion* r = ann_.dataCovering(a);
    if (!r || r->name.empty() || r->address == a || r->kind == DataKind::Lohi) return std::nullopt;
    return r->address;
}

std::string Renderer::labelExpr(Address a) {
    if (const auto base = namedRegionInside(a)) {
        labelUsed_.insert(*base);
        const unsigned off = a - *base;
        return nameFor(*base) + "+" + (off < 10 ? std::to_string(off) : "$" + hexDigits(off, 1));
    }
    auto it = items_.upper_bound(a);
    if (it == items_.begin()) return h4(a);
    --it;
    const Item& item = it->second;
    if (a >= item.start + item.length && !(item.region && item.region->kind == DataKind::Lohi)) return h4(a);
    labelUsed_.insert(item.start);
    if (item.start == a) return nameFor(a);
    const unsigned off = a - item.start;
    return nameFor(item.start) + "+" + (off < 10 ? std::to_string(off) : "$" + hexDigits(off, 1));
}

void Renderer::useEquate(const std::string& name, Address a) {
    if (equateNames_.count(name)) return;
    equateNames_[name] = a;
    equates_[a] = name;
}

std::string Renderer::systemName(Address a) {
    // chip registers, KERNAL entries, RAM vectors and colour RAM keep their names even inside the analysed range (a program
    // whose image reaches $D000-$FFFF still meets the chips and the ROM there), unless code was found at the address or the
    // user named it (a program that banks the ROM out uses the RAM underneath: room_grid beats TKSA)
    if (!o_.c64Names || an_.isCode(a) || ann_.nameAt(a)) return "";
    bool covered = false;
    for (const VariableDoc& v : ann_.variables())
        covered = covered || (a >= v.address && a < v.address + v.size);
    const auto& k = kernalSymbols();
    // a KERNAL jump table entry stays itself even inside a table the program keeps in the RAM underneath (JSR SETLFS, not
    // JSR room_grid+$24: the program calls the ROM with the ROM banked in)
    if (covered && !(a >= 0xFF81 && a < 0xFFF6)) return "";
    if (const auto it = k.find(a); it != k.end() && isValidSymbolName(it->second)) {
        useEquate(it->second, a);
        return it->second;
    }
    if (covered) return "";
    if (a >= 0xD000 && a < 0xE000 && isBaseRegister(a)) {
        const std::string r = C64::registerName(a);
        if (!r.empty() && isValidSymbolName(r)) {
            useEquate(r, a);
            return r;
        }
    }
    if (a >= 0xD800 && a < 0xDC00) {
        useEquate("COLRAM", 0xD800);
        return a == 0xD800 ? std::string("COLRAM") : "COLRAM+$" + hexDigits(a - 0xD800, 1);
    }
    return "";
}

std::string Renderer::symExpr(Address a) {
    if (const std::string n = systemName(a); !n.empty()) return n;
    if (inList(a)) return labelExpr(a);
    if (const auto e = ann_.expressionFor(a)) {
        const std::string base = e->substr(0, e->find('+'));
        if (const auto at = ann_.addressOf(base); at && !inList(*at)) {
            useEquate(base, *at);
            return *e;
        }
        if (const auto at = ann_.addressOf(base); at && inList(*at)) return labelExpr(a);
    }
    if (an_.inRange(a)) {
        // outside this excerpt but inside the analysed program: the name the whole listing gives it, so that an excerpt
        // reads the same as the full listing (an EQU line keeps a verified excerpt assemblable)
        Address base = a;
        std::string suffix;
        if (const auto start = an_.instructionStart(a); start && *start != a) {
            base = *start;
            const unsigned off = a - base;
            suffix = "+" + std::to_string(off);
        }
        const std::string n = nameFor(base);
        useEquate(n, base);
        return n + suffix;
    }
    if (o_.c64Names) {
        const auto& k = kernalSymbols();
        if (const auto it = k.find(a); it != k.end() && isValidSymbolName(it->second)) {
            useEquate(it->second, a);
            return it->second;
        }
        if (a >= 0xD000 && a < 0xE000 && isBaseRegister(a)) {
            const std::string r = C64::registerName(a);
            if (!r.empty() && isValidSymbolName(r)) {
                useEquate(r, a);
                return r;
            }
        }
    }
    return "";
}

void Renderer::collectReferences() {
    for (const RoutineInfo& r : an_.routines()) routineEntries_.insert(r.entry);
    for (const auto& [a, in] : an_.code()) {
        if (!inList(a)) continue;
        if (in.target && inList(*in.target) && systemName(*in.target).empty()) needLabel_.insert(*in.target);
        if (in.reference && inList(*in.reference) && systemName(*in.reference).empty() && !namedRegionInside(*in.reference)) {
            needLabel_.insert(*in.reference);
            if (in.mode == Mode::Absolute || in.mode == Mode::ZeroPage) scalarRefs_.insert(*in.reference);
        }
    }
    for (const auto& [a, p] : an_.pointerImmediates())
        if (inList(a) && inList(p.first) && !namedRegionInside(p.first) && systemName(p.first).empty()) needLabel_.insert(p.first);
    for (const JumpTable& t : an_.jumpTables()) {
        if (!t.followed) continue;
        for (const Address target : t.targets)
            if (inList(target)) needLabel_.insert(target);
    }
    for (const auto& [a, n] : ann_.labels())
        if (inList(a)) needLabel_.insert(a);
    for (const auto& [a, r] : ann_.dataRegions()) {
        if (inList(a)) needLabel_.insert(a);
        if (r.kind == DataKind::Lohi && inList(r.partner)) needLabel_.insert(r.partner);
    }
    for (const Address e : routineEntries_)
        if (inList(e)) needLabel_.insert(e);
}

void Renderer::buildItems() {
    equates_.clear();       // collectReferences only asked; the names in use are collected while the lines are written
    equateNames_.clear();
    // where tables found by the analysis start (and which side they are)
    std::map<Address, std::pair<const JumpTable*, bool>> tableStarts;
    for (const JumpTable& t : an_.jumpTables()) {
        if (!t.followed || !an_.isTableData(t.table)) continue;
        tableStarts[t.table] = {&t, false};
        if (t.split) tableStarts[t.hiTable] = {&t, true};
    }
    std::set<Address> boundaries = needLabel_;
    for (const auto& [a, in] : an_.code()) boundaries.insert(a);
    for (const auto& [a, r] : ann_.dataRegions()) {
        boundaries.insert(a);
        if (r.kind == DataKind::Lohi) boundaries.insert(r.partner);
    }
    for (const auto& [a, t] : tableStarts) boundaries.insert(a);
    for (const auto& [a, c] : ann_.comments()) boundaries.insert(a);       // a commented data byte starts a line
    for (const auto& [a, c] : ann_.blockComments()) boundaries.insert(a);
    for (const VariableDoc& v : ann_.variables()) {                       // a named variable is a line of its own
        boundaries.insert(v.address);
        boundaries.insert(static_cast<Address>(v.address + v.size));
    }
    for (const Address a : scalarRefs_) {                                 // so is a byte used as a single variable
        boundaries.insert(a);
        boundaries.insert(static_cast<Address>(a + 1));
    }

    std::uint32_t a = o_.from;
    while (a < o_.to) {
        const auto at = static_cast<Address>(a);
        Item it;
        it.start = at;
        const Instruction* in = an_.at(at);
        const DataRegion* region = ann_.dataAt(at);
        if (in && a + in->length() <= o_.to && !ann_.dataCovering(at)) {
            it.kind = Item::Code;
            it.length = static_cast<unsigned>(in->length());
        } else if (region) {
            it.kind = Item::Region;
            it.region = region;
            it.hiSide = region->kind == DataKind::Lohi && region->partner == at && region->address != at;
            it.length = std::min<std::uint32_t>(region->length, o_.to - a);
            if (region->kind == DataKind::Bytes) {
                // plain bytes: a variable, a referenced address or a comment inside starts a line of its own (the rest
                // continues as plain bytes), so that a named table inside a large region keeps its name
                if (const auto b = boundaries.upper_bound(at); b != boundaries.end() && *b < a + it.length) it.length = *b - a;
            }
        } else if (const auto ts = tableStarts.find(at); ts != tableStarts.end()) {
            const JumpTable& t = *ts->second.first;
            it.kind = Item::Table;
            it.table = &t;
            it.hiSide = ts->second.second;
            const unsigned n = static_cast<unsigned>(t.slots.size());
            it.length = std::min<std::uint32_t>(t.split ? n : 2 * n, o_.to - a);
            if (t.kind == JumpTable::Kind::Vector) it.length = std::min<std::uint32_t>(2, o_.to - a);
        } else if (a == o_.from && a == 0x0801 && !an_.isCode(at)) {
            const auto stub = parseSysStub(view_, at);
            bool free = stub.has_value();
            for (std::size_t i = 0; free && i < stub->length; ++i)
                free = !an_.isCode(static_cast<Address>(a + i)) && !an_.isTableData(static_cast<Address>(a + i)) &&
                       !ann_.dataCovering(static_cast<Address>(a + i)) && (i == 0 || !needLabel_.count(static_cast<Address>(a + i))) &&
                       a + i < o_.to;
            if (free) {
                it.kind = Item::Basic;
                it.stub = *stub;
                it.length = static_cast<unsigned>(stub->length);
            }
        }
        if (it.length == 0) {
            // plain bytes up to the next boundary: zero runs of 16 and more as .FILL, else up to 8 per line
            std::uint32_t end = o_.to;
            if (const auto b = boundaries.upper_bound(at); b != boundaries.end()) end = std::min<std::uint32_t>(end, *b);
            unsigned zeros = 0;
            while (a + zeros < end && view_.read8(static_cast<Address>(a + zeros)) == 0) ++zeros;
            if (zeros >= 16) {
                it.kind = Item::Fill;
                it.length = zeros;
            } else {
                it.kind = Item::Bytes;
                unsigned n = 0;
                while (a + n < end && n < 8) {
                    // stop before a long zero run so that it becomes a .FILL of its own
                    unsigned z = 0;
                    while (n > 0 && a + n + z < end && view_.read8(static_cast<Address>(a + n + z)) == 0 && z < 16) ++z;
                    if (z >= 16) break;
                    ++n;
                }
                it.length = std::max(1U, n);
            }
        }
        items_[at] = it;
        a += it.length;
    }
}

std::string Renderer::tableEntry(Address target, bool minusOne, Address raw) {
    const std::string e = symExpr(target);
    if (e.empty()) return h4(raw);
    return minusOne ? e + "-1" : e;
}

std::vector<std::string> Renderer::renderCode(const Item& it, unsigned& userComments) {
    const Instruction& orig = *an_.at(it.start);
    DecodeOptions o;
    o.level = CpuLevel::All;
    o.symbolizer = [this](Address a) { return symExpr(a); };
    Instruction in = decode(view_, it.start, o);
    std::vector<std::string> comments;
    std::string text;
    if (!in.canonical) {
        std::string bytes;
        for (std::size_t i = 0; i < orig.bytes.size(); ++i) bytes += (i ? "," : "") + h2(orig.bytes[i]);
        text = ".BYTE " + bytes;
        comments.push_back(orig.text() + " (alternative opcode)");
    } else {
        if (const auto p = an_.pointerImmediates().find(it.start); p != an_.pointerImmediates().end() && !ann_.isConstant(it.start)) {
            const std::string e = symExpr(p->second.first);
            if (!e.empty()) in.operands = std::string("#") + (p->second.second ? "<" : ">") + e;
        }
        text = in.text();
    }
    if (const std::string* c = ann_.commentAt(it.start)) {
        comments.insert(comments.begin(), *c);
        ++userComments;
    }
    if (const auto sm = an_.selfModified().find(it.start); sm != an_.selfModified().end()) {
        std::string w;
        for (const Address x : sm->second) w += (w.empty() ? "" : ", ") + h4(x);
        comments.push_back("operand changed by the code at " + w);
    }
    std::string line = "        " + text;
    const std::string c = commentText(it.start, comments);
    if (!c.empty()) line = pad(line, 40) + "; " + c;
    return {line};
}

std::vector<std::string> Renderer::renderItem(const Item& it, unsigned& userComments, unsigned& instructions) {
    if (it.kind == Item::Code) {
        ++instructions;
        return renderCode(it, userComments);
    }
    std::vector<std::string> lines;
    auto byteAt = [&](unsigned i) { return view_.read8(static_cast<Address>(it.start + i)); };
    auto emitList = [&](const std::string& directive, const std::vector<std::string>& values, unsigned perLine,
                        const std::vector<std::string>* rowComments = nullptr) {
        for (std::size_t i = 0; i < values.size(); i += perLine) {
            std::string l = "        " + directive + " ";
            for (std::size_t k = i; k < values.size() && k < i + perLine; ++k) l += (k > i ? "," : "") + values[k];
            if (rowComments && i / perLine < rowComments->size()) l = pad(l, 40) + "; " + (*rowComments)[i / perLine];
            lines.push_back(l);
        }
    };
    auto bytesList = [&](unsigned from, unsigned to) {
        std::vector<std::string> v;
        for (unsigned i = from; i < to; ++i) v.push_back(h2(byteAt(i)));
        return v;
    };
    switch (it.kind) {
        case Item::Fill:
            lines.push_back("        .FILL " + std::to_string(it.length));
            break;
        case Item::Bytes:
            emitList(".BYTE", bytesList(0, it.length), 8);
            break;
        case Item::Basic: {
            const std::string target = symExpr(it.stub.target);
            lines.push_back("        .BASIC " + std::to_string(it.stub.line) + "," + (target.empty() ? std::to_string(it.stub.target) : target));
            break;
        }
        case Item::Table: {
            const JumpTable& t = *it.table;
            std::vector<std::string> v;
            if (t.kind == JumpTable::Kind::Vector) {
                v.push_back(tableEntry(t.targets[0], false, t.targets[0]));
                emitList(".WORD", v, 4);
                break;
            }
            for (std::size_t i = 0; i < t.slots.size(); ++i) {
                const Address raw = static_cast<Address>(t.targets[i] - (t.minusOne ? 1 : 0));
                v.push_back(tableEntry(t.targets[i], t.minusOne, raw));
            }
            if (t.split) emitList(it.hiSide ? ".HI" : ".LO", v, 4);
            else emitList(".WORD", v, 4);
            break;
        }
        case Item::Region: {
            const DataRegion& r = *it.region;
            const unsigned n = it.length;
            switch (r.kind) {
                case DataKind::Bytes:
                    emitList(".BYTE", bytesList(0, n), 8);
                    break;
                case DataKind::Words:
                case DataKind::Pointers:
                case DataKind::RtsPointers: {
                    std::vector<std::string> v;
                    for (unsigned i = 0; i + 1 < n; i += 2) {
                        const auto w = static_cast<Address>(byteAt(i) | (byteAt(i + 1) << 8));
                        if (r.kind == DataKind::Words) v.push_back(h4(w));
                        else if (r.kind == DataKind::Pointers) v.push_back(tableEntry(w, false, w));
                        else v.push_back(tableEntry(static_cast<Address>(w + 1), true, w));
                    }
                    emitList(".WORD", v, r.kind == DataKind::Words ? 8 : 4);
                    if (n % 2) emitList(".BYTE", bytesList(n - 1, n), 8);
                    break;
                }
                case DataKind::Lohi: {
                    std::vector<std::string> v;
                    for (unsigned i = 0; i < n; ++i) {
                        const auto w = static_cast<Address>(view_.read8(static_cast<Address>(r.address + i)) |
                                                            (view_.read8(static_cast<Address>(r.partner + i)) << 8));
                        v.push_back(tableEntry(static_cast<Address>(w + (r.minusOne ? 1 : 0)), r.minusOne, w));
                    }
                    emitList(it.hiSide ? ".HI" : ".LO", v, 4);
                    break;
                }
                case DataKind::Text: {
                    std::vector<std::string> v;
                    std::string run;
                    for (unsigned i = 0; i < n; ++i) {
                        const Byte b = byteAt(i);
                        if (stringSafe(static_cast<char>(b))) {
                            run += static_cast<char>(b);
                            continue;
                        }
                        if (!run.empty()) v.push_back("\"" + run + "\""), run.clear();
                        v.push_back(h2(b));
                    }
                    if (!run.empty()) v.push_back("\"" + run + "\"");
                    // lines of at most about 60 characters
                    std::string l;
                    for (const std::string& x : v) {
                        if (!l.empty() && l.size() + x.size() > 60) {
                            lines.push_back("        .BYTE " + l);
                            l.clear();
                        }
                        l += (l.empty() ? "" : ",") + x;
                    }
                    if (!l.empty()) lines.push_back("        .BYTE " + l);
                    break;
                }
                case DataKind::Screen: {
                    static const auto reverse = [] {
                        std::map<Byte, char> m;
                        for (int c = 0x20; c < 0x7F; ++c) {
                            if (!stringSafe(static_cast<char>(c))) continue;
                            if (const auto sc = asciiToScreenCode(static_cast<char>(c)); sc && !m.count(*sc)) m[*sc] = static_cast<char>(c);
                        }
                        return m;
                    }();
                    unsigned i = 0;
                    while (i < n) {
                        std::string run;
                        unsigned j = i;
                        while (j < n && reverse.count(byteAt(j)) && run.size() < 50) run += reverse.at(byteAt(j++));
                        if (run.size() >= 2) {
                            lines.push_back("        .SCREEN \"" + run + "\"");
                            i = j;
                            continue;
                        }
                        unsigned k = i;
                        std::vector<std::string> v;
                        while (k < n && v.size() < 8) {
                            if (k > i && reverse.count(byteAt(k)) && k + 1 < n && reverse.count(byteAt(k + 1))) break;
                            v.push_back(h2(byteAt(k++)));
                        }
                        emitList(".BYTE", v, 8);
                        i = k;
                    }
                    break;
                }
                case DataKind::Sprites: {
                    std::vector<std::string> rows;
                    unsigned i = 0;
                    for (; i + 3 <= n; i += 3) {
                        if ((i % 64) == 63) break;
                        std::string pattern;
                        for (unsigned k = 0; k < 3; ++k)
                            for (int bit = 7; bit >= 0; --bit) pattern += ((byteAt(i + k) >> bit) & 1) ? '#' : '.';
                        lines.push_back(pad("        .BYTE " + h2(byteAt(i)) + "," + h2(byteAt(i + 1)) + "," + h2(byteAt(i + 2)), 40) + "; " + pattern);
                    }
                    if (i < n) emitList(".BYTE", bytesList(i, n), 8);
                    break;
                }
                case DataKind::Charset: {
                    for (unsigned i = 0; i < n; i += 8) {
                        std::vector<std::string> v = bytesList(i, std::min(n, i + 8));
                        std::string l = "        .BYTE ";
                        for (std::size_t k = 0; k < v.size(); ++k) l += (k ? "," : "") + v[k];
                        lines.push_back(pad(l, 48) + "; " + h2(i / 8));
                    }
                    break;
                }
            }
            break;
        }
        case Item::Code: break;
    }
    std::vector<std::string> parts;
    if (const std::string* c = ann_.commentAt(it.start)) parts.push_back(*c);
    if (const VariableDoc* v = ann_.variableAt(it.start); v && !v->comment.empty()) parts.push_back(v->comment);
    if (it.region && !it.region->comment.empty() && !it.hiSide && it.region->address == it.start) parts.push_back(it.region->comment);
    const std::string c = commentText(it.start, parts);
    if (!c.empty() && !lines.empty()) {
        if (lines[0].find(';') == std::string::npos) lines[0] = pad(lines[0], 40) + "; " + c;
        else lines.insert(lines.begin(), "; " + c);
    }
    return lines;
}

ListingResult Renderer::run() {
    ListingResult res;
    collectReferences();
    buildItems();
    // first pass: the lines of every item (this also finds the labels and names that are used)
    std::map<Address, std::vector<std::string>> body;
    for (const auto& [a, it] : items_) body[a] = renderItem(it, res.userComments, res.instructions);

    std::string out;
    const bool header = o_.equates || o_.verify;
    if (header) {
        out += "; " + (ann_.title().empty() ? std::string("listing") : ann_.title()) + "\n";
        out += "; generated by m6510tool " + std::string(kVersion) + " from the bytes " + h4(o_.from) + "-" + h4(o_.to - 1) + "\n\n";
        out += "        .CPU 6510X\n";
        if (!equates_.empty()) out += "\n";
        for (const auto& [a, n] : equates_) {
            std::string l = pad(n, 16) + "= " + h4(a);
            if (const VariableDoc* v = ann_.variableAt(a); v && !v->comment.empty()) l = pad(l, 40) + "; " + v->comment;
        else if (o_.c64Names && kernalSymbols().count(a)) l = pad(l, 40) + "; KERNAL";
            else if (a >= 0xD000 && a < 0xE000 && isBaseRegister(a) && n == C64::registerName(a)) l = pad(l, 40) + "; chip register";
            out += l + "\n";
        }
        out += "\n        * = " + h4(o_.from) + "\n";
    }
    std::set<std::string> placeholders;
    for (const auto& [a, it] : items_) {
        const bool routine = routineEntries_.count(a) != 0 && it.kind == Item::Code;
        if (routine) {
            out += "\n; " + std::string(60, '-') + "\n";
            if (const RoutineDoc* d = ann_.routineAt(a)) {
                out += "; " + nameFor(a) + (d->summary.empty() ? "" : ": " + d->summary) + "\n";
                if (!d->inputs.empty()) out += ";   in:       " + d->inputs + "\n";
                if (!d->outputs.empty()) out += ";   out:      " + d->outputs + "\n";
                if (!d->clobbers.empty()) out += ";   clobbers: " + d->clobbers + "\n";
                if (!d->confidence.empty()) out += ";   confidence: " + d->confidence + "\n";
            } else {
                out += "; " + nameFor(a) + " (not documented yet)\n";
            }
        } else if (it.kind != Item::Code && (labelUsed_.count(a) || ann_.nameAt(a))) {
            out += "\n";
        }
        if (const std::string* b = ann_.blockCommentAt(a)) {
            std::string text = *b, line;
            for (char c : text + "\n") {
                if (c == '\n') {
                    out += "; " + line + "\n";
                    line.clear();
                } else {
                    line += c;
                }
            }
        }
        if (labelUsed_.count(a) || needLabel_.count(a) || ann_.nameAt(a)) {
            const std::string n = nameFor(a);
            if (!ann_.nameAt(a)) placeholders.insert(n);
            out += n + ":\n";
        }
        for (const std::string& l : body[a]) out += l + "\n";
    }
    res.unnamed = static_cast<unsigned>(placeholders.size());

    if (o_.maxChars > 0 && out.size() > o_.maxChars) {
        const std::size_t cut = out.rfind('\n', o_.maxChars);
        out.resize(cut == std::string::npos ? 0 : cut + 1);
        out += "; ... (truncated)\n";
        res.truncated = true;
        res.problem = "truncated listing: not verified";
        res.text = out;
        return res;
    }
    res.text = out;
    if (o_.verify) {
        AssemblerOptions ao;
        ao.origin = o_.from;
        ao.level = CpuLevel::All;
        const AssemblyResult r = assemble(out, ao);
        if (!r.ok()) {
            res.problem = "the listing does not assemble: line " + std::to_string(r.errors[0].line) + ": " + r.errors[0].message;
        } else if (r.lowest() != o_.from || r.end() != o_.to) {
            res.problem = "the listing covers " + h4(r.lowest()) + "-" + h4(r.end() - 1) + " instead of " + h4(o_.from) + "-" + h4(o_.to - 1);
        } else {
            const std::vector<Byte> bytes = r.flatten();
            res.verified = true;
            for (std::uint32_t a = o_.from; a < o_.to; ++a) {
                const Byte want = view_.read8(static_cast<Address>(a)), got = bytes[a - o_.from];
                if (want != got) {
                    res.verified = false;
                    res.problem = "byte at " + h4(a) + ": the listing gives " + h2(got) + ", the program has " + h2(want);
                    break;
                }
            }
        }
    }
    return res;
}

}  // namespace

ListingResult renderListing(const Memory& view, const Analysis& analysis, const Annotations& annotations, const ListingOptions& options) {
    ListingOptions o = options;
    o.to = std::min<std::uint32_t>(o.to, 0x10000);
    if (o.to <= o.from) {
        ListingResult r;
        r.problem = "empty range";
        return r;
    }
    return Renderer(view, analysis, annotations, o).run();
}

}  // namespace m6510
