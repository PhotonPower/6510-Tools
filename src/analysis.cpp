#include "m6510/analysis.hpp"

#include <algorithm>
#include <functional>

#include "m6510/decoder.hpp"
#include "m6510/format.hpp"
#include "m6510/kernal.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

enum : std::uint8_t { kUnknown = 0, kStart = 1, kOperand = 2, kTable = 3 };

std::string baseMnemonic(const Instruction& in) {
    std::string m = in.mnemonic;
    if (const auto dot = m.find('.'); dot != std::string::npos && dot > 0) m.resize(dot);
    return m;
}

bool hasMemoryOperand(const Instruction& in) {
    return in.valid && in.reference && in.mode != Mode::Immediate && in.mode != Mode::Implied && in.mode != Mode::Accumulator &&
           in.mode != Mode::Relative;
}

bool isIndexedAbsolute(const Instruction& in) { return in.mode == Mode::AbsoluteX || in.mode == Mode::AbsoluteY; }

bool isLoad(const std::string& m) { return m == "LDA" || m == "LDX" || m == "LDY"; }
bool isStore(const std::string& m) { return m == "STA" || m == "STX" || m == "STY"; }
char loadRegister(const std::string& m) { return m[2]; }   // LDA -> 'A'
char storeRegister(const std::string& m) { return m[2]; }  // STX -> 'X'

bool petsciiPrintable(Byte b) {
    if (b >= 0x20 && b <= 0x5F) return true;
    if (b >= 0xC1 && b <= 0xDA) return true;
    switch (b) {
        case 0x05: case 0x0D: case 0x11: case 0x12: case 0x13: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x81: case 0x8D: case 0x90: case 0x91: case 0x92: case 0x93: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            return true;
        default:
            return false;
    }
}
bool petsciiLetterOrDigit(Byte b) { return (b >= 0x41 && b <= 0x5A) || (b >= 0xC1 && b <= 0xDA) || (b >= 0x30 && b <= 0x39); }

}  // namespace

const char* toString(DataGuess::Kind k) {
    switch (k) {
        case DataGuess::Kind::Text: return "text";
        case DataGuess::Kind::Screen: return "screen";
        case DataGuess::Kind::Zeros: return "zeros";
        case DataGuess::Kind::Words: return "words";
        case DataGuess::Kind::Pointers: return "pointers";
        case DataGuess::Kind::Sprites: return "sprites";
        case DataGuess::Kind::Charset: return "charset";
        case DataGuess::Kind::Basic: return "basic";
    }
    return "?";
}

bool Analysis::writesMemory(const Instruction& in) {
    if (!hasMemoryOperand(in)) return false;
    static const std::set<std::string> w = {"STA", "STX", "STY", "SAX", "SHA", "SHX", "SHY", "TAS", "ASL", "LSR",
                                            "ROL", "ROR", "INC", "DEC", "SLO", "RLA", "SRE", "RRA", "DCP", "ISC"};
    return w.count(baseMnemonic(in)) != 0;
}

bool Analysis::readsMemory(const Instruction& in) {
    if (!hasMemoryOperand(in)) return false;
    static const std::set<std::string> notRead = {"STA", "STX", "STY", "SAX", "SHA", "SHX", "SHY", "TAS", "JMP", "JSR"};
    return notRead.count(baseMnemonic(in)) == 0;
}

Analysis::Analysis(const Memory& view, Address from, std::uint32_t to)
    : mem_(view), from_(from), to_(std::min<std::uint32_t>(std::max<std::uint32_t>(to, from), 0x10000)),
      state_(to_ - from_, kUnknown) {}

void Analysis::addEntry(Address address) {
    if (inRange(address)) entries_.insert(address);
}

void Analysis::addPointerTable(Address table, unsigned bytes, bool minusOne) {
    JumpTable t;
    t.kind = JumpTable::Kind::Pointers;
    t.table = table;
    t.minusOne = minusOne;
    for (unsigned i = 0; i + 1 < bytes; i += 2) {
        const auto slot = static_cast<Address>(table + i);
        const auto target = static_cast<Address>(mem_.read16(slot) + (minusOne ? 1 : 0));
        t.slots.push_back(slot);
        t.targets.push_back(target);
        if (inRange(target)) tableTargets_.insert(target);
    }
    t.bytes = static_cast<unsigned>(t.slots.size() * 2);
    if (!t.slots.empty()) tables_.push_back(std::move(t));
}

void Analysis::addLohiTable(Address lo, Address hi, unsigned count, bool minusOne) {
    JumpTable t;
    t.kind = JumpTable::Kind::Lohi;
    t.table = lo;
    t.hiTable = hi;
    t.split = true;
    t.minusOne = minusOne;
    for (unsigned i = 0; i < count; ++i) {
        const auto target = static_cast<Address>(((mem_.read8(static_cast<Address>(hi + i)) << 8) | mem_.read8(static_cast<Address>(lo + i))) +
                                                 (minusOne ? 1 : 0));
        t.slots.push_back(static_cast<Address>(lo + i));
        t.targets.push_back(target);
        if (inRange(target)) tableTargets_.insert(target);
    }
    t.bytes = count * 2;
    if (!t.slots.empty()) tables_.push_back(std::move(t));
}

const Instruction* Analysis::at(Address a) const {
    const auto it = code_.find(a);
    return it == code_.end() ? nullptr : &it->second;
}

bool Analysis::isCode(Address a) const { return inRange(a) && (state(a) == kStart || state(a) == kOperand); }
bool Analysis::isTableData(Address a) const { return inRange(a) && state(a) == kTable; }

std::optional<Address> Analysis::instructionStart(Address a) const {
    if (!isCode(a)) return std::nullopt;
    for (unsigned back = 0; back < 3 && a >= from_ + back; ++back) {
        const auto p = static_cast<Address>(a - back);
        if (const Instruction* in = at(p); in && p + in->length() > a) return p;
    }
    return std::nullopt;
}

std::size_t Analysis::codeBytes() const {
    std::size_t n = 0;
    for (const auto& [a, in] : code_) n += in.length();
    return n;
}

std::optional<Address> Analysis::previous(Address a) const {
    for (unsigned len = 1; len <= 3; ++len) {
        if (a < from_ + len) break;
        const auto p = static_cast<Address>(a - len);
        if (const Instruction* in = at(p); in && in->length() == len) return p;
    }
    return std::nullopt;
}

bool Analysis::place(const Instruction& in) {
    const Address a = in.address;
    if (static_cast<std::uint32_t>(a) + in.length() > to_) return false;
    for (std::size_t i = 0; i < in.length(); ++i) {
        const auto x = static_cast<Address>(a + i);
        if (state(x) != kUnknown || (isData_ && isData_(x))) return false;
    }
    state_[a - from_] = kStart;
    for (std::size_t i = 1; i < in.length(); ++i) state_[a + i - from_] = kOperand;
    code_[a] = in;
    if (in.reference) refs_.insert(*in.reference);
    return true;
}

void Analysis::follow(std::vector<Address>& work) {
    DecodeOptions o;
    o.level = CpuLevel::All;
    while (!work.empty()) {
        std::uint32_t a = work.back();
        work.pop_back();
        while (inRange(a) && state(static_cast<Address>(a)) == kUnknown) {
            const Instruction in = decode(mem_, static_cast<Address>(a), o);
            if (!place(in)) break;
            const std::uint32_t next = a + static_cast<std::uint32_t>(in.length());
            bool goOn = true;
            switch (in.flow) {
                case FlowKind::Normal: break;
                case FlowKind::CondJump:
                    jumpTargets_.insert(*in.target);
                    work.push_back(*in.target);
                    break;
                case FlowKind::Call:
                    if (in.target && inRange(*in.target)) {
                        callTargets_.insert(*in.target);
                        callers_[*in.target].insert(in.address);
                        work.push_back(*in.target);
                    }
                    break;
                case FlowKind::Jump:
                    if (in.mode == Mode::Absolute && in.target) {
                        jumpTargets_.insert(*in.target);
                        work.push_back(*in.target);
                    }
                    goOn = false;
                    break;
                case FlowKind::Return:
                case FlowKind::Halt: goOn = false; break;
            }
            if (!goOn) break;
            a = next;
        }
    }
}

unsigned Analysis::tableLength(Address lo, Address hi, unsigned stride, bool minusOne) const {
    unsigned max = 128;
    if (stride == 1) {  // split tables: neither may run into the other
        if (hi > lo) max = std::min<unsigned>(max, static_cast<unsigned>(hi - lo));
        else if (lo > hi) max = std::min<unsigned>(max, static_cast<unsigned>(lo - hi));
        else return 0;
    }
    unsigned n = 0;
    for (; n < max; ++n) {
        const std::uint32_t sl = lo + n * stride;
        const std::uint32_t sh = stride == 1 ? hi + n : sl + 1;
        if (!inRange(sl) || !inRange(sh)) break;
        const auto l = static_cast<Address>(sl), h = static_cast<Address>(sh);
        if (state(l) == kStart || state(l) == kOperand || state(h) == kStart || state(h) == kOperand) break;
        if (isData_ && (isData_(l) || isData_(h))) break;
        if (n > 0 && (refs_.count(l) || (stride == 1 && refs_.count(h)))) break;  // another table or variable starts here
        const auto target = static_cast<Address>(((mem_.read8(h) << 8) | mem_.read8(l)) + (minusOne ? 1 : 0));
        if (!inRange(target) || state(target) == kOperand || state(target) == kTable) break;
        if (target == l || target == h) break;
    }
    return n;
}

bool Analysis::addTable(JumpTable t, std::vector<Address>& work) {
    if (t.slots.empty()) return false;
    for (const JumpTable& x : tables_)
        if (x.table == t.table && x.kind == t.kind) return false;
    for (std::size_t i = 0; i < t.slots.size(); ++i) {
        const Address s = t.slots[i];
        state_[s - from_] = kTable;
        if (t.split) state_[t.hiTable + i - from_] = kTable;
        else state_[static_cast<Address>(s + 1) - from_] = kTable;
        const Address target = t.targets[i];
        if (inRange(target)) {
            tableTargets_.insert(target);
            jumpTargets_.insert(target);
            work.push_back(target);
        }
    }
    tables_.push_back(std::move(t));
    return true;
}

void Analysis::detectTables(std::vector<Address>& work) {
    std::vector<Address> candidates;
    for (const auto& [a, in] : code_)
        if (!handledJumps_.count(a) && (in.mnemonic == "RTS" || (in.mnemonic == "JMP" && in.mode == Mode::Indirect))) candidates.push_back(a);
    for (const Address a : candidates) {
        handledJumps_.insert(a);
        const Instruction& in = code_.at(a);
        auto split = [&](JumpTable::Kind kind, Address lo, Address hi, bool minusOne) {
            JumpTable t;
            t.kind = kind;
            t.jump = a;
            t.minusOne = minusOne;
            const bool words = hi == static_cast<Address>(lo + 1);
            const unsigned n = tableLength(lo, hi, words ? 2 : 1, minusOne);
            if (n == 0) return;
            t.table = lo;
            t.split = !words;
            t.hiTable = words ? 0 : hi;
            for (unsigned i = 0; i < n; ++i) {
                const auto sl = static_cast<Address>(words ? lo + 2 * i : lo + i);
                const auto sh = static_cast<Address>(words ? sl + 1 : hi + i);
                t.slots.push_back(sl);
                t.targets.push_back(static_cast<Address>(((mem_.read8(sh) << 8) | mem_.read8(sl)) + (minusOne ? 1 : 0)));
            }
            t.bytes = 2 * n;
            addTable(std::move(t), work);
        };
        if (in.mnemonic == "RTS") {
            // LDA hi,X / PHA / LDA lo,X / PHA / RTS
            const auto p1 = previous(a);
            const auto p2 = p1 ? previous(*p1) : std::nullopt;
            const auto p3 = p2 ? previous(*p2) : std::nullopt;
            const auto p4 = p3 ? previous(*p3) : std::nullopt;
            if (!p4) continue;
            const Instruction &i1 = code_.at(*p1), &i2 = code_.at(*p2), &i3 = code_.at(*p3), &i4 = code_.at(*p4);
            if (i1.mnemonic != "PHA" || i3.mnemonic != "PHA" || baseMnemonic(i2) != "LDA" || baseMnemonic(i4) != "LDA") continue;
            if (!isIndexedAbsolute(i2) || i2.mode != i4.mode || !i2.reference || !i4.reference) continue;
            split(JumpTable::Kind::Rts, *i2.reference, *i4.reference, true);
            continue;
        }
        // JMP (vector)
        const Address v = *in.reference;
        bool written = false;
        for (const auto& [x, w] : code_)
            if (writesMemory(w) && w.reference && (*w.reference == v || *w.reference == static_cast<Address>(v + 1))) written = true;
        if (!written) {
            if (inRange(v) && inRange(static_cast<std::uint32_t>(v) + 1) && state(v) == kUnknown && state(static_cast<Address>(v + 1)) == kUnknown &&
                !(isData_ && isData_(v))) {
                const Address target = mem_.read16(v);
                if (inRange(target)) {
                    JumpTable t;
                    t.kind = JumpTable::Kind::Vector;
                    t.jump = a;
                    t.table = v;
                    t.bytes = 2;
                    t.slots = {v};
                    t.targets = {target};
                    addTable(std::move(t), work);
                }
            }
            continue;
        }
        // LDA lo,X / STA v / LDA hi,X / STA v+1 / JMP (v)   (in either order, up to 8 instructions back)
        std::optional<Address> lo, hi;
        Mode index = Mode::Implied;
        auto p = previous(a);
        for (int k = 0; k < 8 && p; ++k, p = previous(*p)) {
            const Instruction& st = code_.at(*p);
            if (!isStore(baseMnemonic(st)) || !st.reference) continue;
            const auto q = previous(*p);
            if (!q) continue;
            const Instruction& ld = code_.at(*q);
            if (!isLoad(baseMnemonic(ld)) || loadRegister(baseMnemonic(ld)) != storeRegister(baseMnemonic(st)) || !isIndexedAbsolute(ld) || !ld.reference) continue;
            if (*st.reference == v && !lo) lo = *ld.reference, index = ld.mode;
            else if (*st.reference == static_cast<Address>(v + 1) && !hi && (index == Mode::Implied || index == ld.mode)) hi = *ld.reference;
            if (lo && hi) break;
        }
        if (lo && hi) split(JumpTable::Kind::Lohi, *lo, *hi, false);
    }
}

void Analysis::detectImmediatePairs(std::vector<Address>* work) {
    for (const auto& [a, in] : code_) {
        const std::string m = baseMnemonic(in);
        if (in.mode != Mode::Immediate || !isLoad(m) || immediates_.count(a)) continue;
        // up to four following instructions in a straight line
        std::vector<const Instruction*> seq;
        std::uint32_t next = a + static_cast<std::uint32_t>(in.length());
        for (int k = 0; k < 4 && inRange(next); ++k) {
            const Instruction* n = at(static_cast<Address>(next));
            if (!n || n->flow != FlowKind::Normal) break;
            seq.push_back(n);
            next += static_cast<std::uint32_t>(n->length());
        }
        auto take = [&](const Instruction& first, const Instruction& second, bool firstIsLow) {
            const unsigned l = firstIsLow ? first.value : second.value, h = firstIsLow ? second.value : first.value;
            const auto ptr = static_cast<Address>(l | (h << 8));
            if (!inRange(ptr) || immediates_.count(second.address)) return false;
            immediates_[first.address] = {ptr, firstIsLow};
            immediates_[second.address] = {ptr, !firstIsLow};
            refs_.insert(ptr);
            return true;
        };
        bool done = false;
        // register pair: LDA/LDX #lo ... LDY #hi (the KERNAL convention), without a reload in between
        if (m == "LDA" || m == "LDX") {
            for (const Instruction* n : seq) {
                const std::string nm = baseMnemonic(*n);
                if (nm == m) break;
                if (nm == "LDY" && n->mode == Mode::Immediate) {
                    done = take(in, *n, true);
                    break;
                }
            }
        }
        if (done) continue;
        // pointer set-up: LD? #a / ST? z / LD? #b / ST? z+1 (or z+1 first)
        if (seq.size() >= 3) {
            const Instruction &s1 = *seq[0], &l2 = *seq[1], &s2 = *seq[2];
            const std::string s1m = baseMnemonic(s1), l2m = baseMnemonic(l2), s2m = baseMnemonic(s2);
            if (isStore(s1m) && storeRegister(s1m) == loadRegister(m) && isLoad(l2m) && l2.mode == Mode::Immediate && isStore(s2m) &&
                storeRegister(s2m) == loadRegister(l2m) && s1.reference && s2.reference && s1.mode != Mode::AbsoluteX &&
                s1.mode != Mode::AbsoluteY && s1.mode != Mode::ZeroPageX && s1.mode != Mode::ZeroPageY) {
                bool taken = false;
                Address vector = 0;
                if (*s2.reference == static_cast<Address>(*s1.reference + 1)) taken = take(in, l2, true), vector = *s1.reference;
                else if (*s1.reference == static_cast<Address>(*s2.reference + 1)) taken = take(in, l2, false), vector = *s2.reference;
                // an interrupt handler (IRQ, BRK, NMI through the KERNAL's RAM vectors or the hardware vectors) is an entry point
                if (taken && (vector == 0x0314 || vector == 0x0316 || vector == 0x0318 || vector == 0xFFFE || vector == 0xFFFA)) {
                    const Address handler = immediates_.at(a).first;
                    vectorEntries_[handler] = vector;
                    if (work && !at(handler)) {
                        entries_.insert(handler);
                        work->push_back(handler);
                    }
                }
            }
        }
    }
}

void Analysis::detectSelfModification() {
    for (const auto& [a, in] : code_) {
        if (!writesMemory(in) || !in.reference) continue;
        if (in.mode != Mode::Absolute && in.mode != Mode::ZeroPage) continue;
        if (const auto start = instructionStart(*in.reference)) selfMod_[*start].insert(a);
    }
}

void Analysis::scanPointerTables() {
    // runs of at least three words that all point to instruction starts: a table of code addresses, not followed yet
    std::uint32_t a = from_;
    while (a + 6 <= to_) {
        unsigned n = 0;
        for (;; ++n) {
            const std::uint32_t s = a + 2 * n;
            if (s + 2 > to_ || state(static_cast<Address>(s)) != kUnknown || state(static_cast<Address>(s + 1)) != kUnknown) break;
            if (isData_ && (isData_(static_cast<Address>(s)) || isData_(static_cast<Address>(s + 1)))) break;
            if (!at(mem_.read16(static_cast<Address>(s)))) break;
        }
        if (n >= 3) {
            JumpTable t;
            t.kind = JumpTable::Kind::Pointers;
            t.table = static_cast<Address>(a);
            t.followed = false;
            for (unsigned i = 0; i < n; ++i) {
                t.slots.push_back(static_cast<Address>(a + 2 * i));
                t.targets.push_back(mem_.read16(static_cast<Address>(a + 2 * i)));
            }
            t.bytes = 2 * n;
            tables_.push_back(std::move(t));
            a += 2 * n;
        } else {
            ++a;
        }
    }
}

void Analysis::analyzeFlow() {
    std::vector<Address> work(entries_.begin(), entries_.end());
    work.insert(work.end(), tableTargets_.begin(), tableTargets_.end());
    do {
        follow(work);
        detectTables(work);
        detectImmediatePairs(&work);
    } while (!work.empty());
    detectSelfModification();
    scanPointerTables();
}

void Analysis::analyzeLinear(Address from, std::uint32_t to) {
    DecodeOptions o;
    o.level = CpuLevel::All;
    std::uint32_t a = std::max<std::uint32_t>(from, from_);
    to = std::min(to, to_);
    while (a < to) {
        if (state(static_cast<Address>(a)) != kUnknown) {
            ++a;
            continue;
        }
        const Instruction in = decode(mem_, static_cast<Address>(a), o);
        if (a + in.length() > to || !place(in)) {
            ++a;
            continue;
        }
        if (in.target && inRange(*in.target)) {
            if (in.flow == FlowKind::Call) {
                callTargets_.insert(*in.target);
                callers_[*in.target].insert(in.address);
            } else {
                jumpTargets_.insert(*in.target);
            }
        }
        a += static_cast<std::uint32_t>(in.length());
    }
    detectImmediatePairs(nullptr);
    selfMod_.clear();
    detectSelfModification();
}

std::vector<DataGuess> Analysis::guessData(const std::function<bool(Address)>& claimed) const {
    std::vector<DataGuess> out;
    std::vector<bool> used(state_.size(), false);
    auto isFree = [&](std::uint32_t a) {
        return inRange(a) && state(static_cast<Address>(a)) == kUnknown && !used[a - from_] && !(claimed && claimed(static_cast<Address>(a)));
    };
    auto freeRun = [&](Address start, unsigned max) {
        unsigned n = 0;
        while (n < max && isFree(start + n)) ++n;
        return n;
    };
    auto add = [&](DataGuess g) {
        for (unsigned i = 0; i < g.length; ++i) used[g.start + i - from_] = true;
        out.push_back(std::move(g));
    };
    // a BASIC program at $0801
    if (from_ == 0x0801 && isFree(0x0801)) {
        std::string error;
        const auto lines = detokenizeBasic(mem_, 0x0801, &error, 1000);
        if (!lines.empty() && error.empty()) {
            const std::uint32_t end = static_cast<std::uint32_t>(lines.back().next) + 2;
            const unsigned n = freeRun(0x0801, static_cast<unsigned>(end - 0x0801));
            if (n == end - 0x0801) {
                DataGuess g;
                g.kind = DataGuess::Kind::Basic;
                g.start = 0x0801;
                g.length = n;
                g.reason = std::to_string(lines.size()) + " BASIC line(s)";
                g.preview = std::to_string(lines[0].number) + " " + lines[0].text;
                add(std::move(g));
            }
        }
    }
    // hints from the code: sprite pointers, $D018, copies into the screen
    for (const auto& [a, in] : code_) {
        const std::string m = baseMnemonic(in);
        if (writesMemory(in) && in.reference && in.mode == Mode::Absolute) {
            const auto p = previous(a);
            const Instruction* ld = p ? at(*p) : nullptr;
            if (!ld || ld->mode != Mode::Immediate || !isLoad(baseMnemonic(*ld)) || loadRegister(baseMnemonic(*ld)) != storeRegister(m)) continue;
            const Address r = *in.reference;
            if (r >= 0x07F8 && r <= 0x07FF) {
                const auto block = static_cast<Address>(ld->value * 64);
                const unsigned n = freeRun(block, 64);
                if (n >= 63) add({DataGuess::Kind::Sprites, block, n, "sprite pointer $" + hexDigits(r, 4) + " = " + std::to_string(ld->value) + " (at $" + hexDigits(a, 4) + ")", ""});
            } else if (r == 0xD018) {
                const auto base = static_cast<Address>(((ld->value >> 1) & 7) * 0x800);
                if (base == 0x1000 || base == 0x1800) continue;  // the character ROM in bank 0
                const unsigned n = freeRun(base, 0x800);
                if (n >= 64) add({DataGuess::Kind::Charset, base, n, "character set chosen with $D018 = $" + hexDigits(ld->value, 2) + " (at $" + hexDigits(a, 4) + ")", ""});
            }
        }
        if (m == "LDA" && isIndexedAbsolute(in) && in.reference && isFree(*in.reference)) {
            std::uint32_t next = a + static_cast<std::uint32_t>(in.length());
            for (int k = 0; k < 3 && inRange(next); ++k) {
                const Instruction* n = at(static_cast<Address>(next));
                if (!n) break;
                if (baseMnemonic(*n) == "STA" && isIndexedAbsolute(*n) && n->reference && *n->reference >= 0x0400 && *n->reference < 0x07E8) {
                    const Address s = *in.reference;
                    unsigned len = 0;
                    std::string preview;
                    while (isFree(s + len) && len < 1000 && (len == 0 || !refs_.count(static_cast<Address>(s + len)))) {
                        const Byte b = mem_.read8(static_cast<Address>(s + len));
                        if ((b & 0x7F) > 0x3F) break;
                        if (preview.size() < 40) preview += screenCodeToChar(b);
                        ++len;
                    }
                    if (len >= 3) add({DataGuess::Kind::Screen, s, len, "copied into the screen by $" + hexDigits(n->address, 4), preview});
                    break;
                }
                next += static_cast<std::uint32_t>(n->length());
            }
        }
    }
    // word tables: LDA tab,X and LDA tab+1,X
    std::set<Address> wordTables;
    for (const auto& [a, in] : code_) {
        if (!isIndexedAbsolute(in) || !in.reference || !readsMemory(in)) continue;
        for (const auto& [b, other] : code_)
            if (isIndexedAbsolute(other) && other.reference && *other.reference == static_cast<Address>(*in.reference + 1) && readsMemory(other))
                wordTables.insert(*in.reference);
    }
    // sequential scan
    std::uint32_t a = from_;
    while (a < to_) {
        if (!isFree(a)) {
            ++a;
            continue;
        }
        const auto at16 = static_cast<Address>(a);
        // zero run
        unsigned z = 0;
        while (isFree(a + z) && mem_.read8(static_cast<Address>(a + z)) == 0) ++z;
        if (z >= 8) {
            add({DataGuess::Kind::Zeros, at16, z, std::to_string(z) + " zero bytes", ""});
            a += z;
            continue;
        }
        if (wordTables.count(at16)) {
            unsigned n = 0;
            bool allCode = true;
            while (isFree(a + 2 * n) && isFree(a + 2 * n + 1) && (n == 0 || !refs_.count(static_cast<Address>(a + 2 * n)))) {
                if (!at(mem_.read16(static_cast<Address>(a + 2 * n)))) allCode = false;
                ++n;
                if (n >= 128) break;
            }
            if (n >= 2) {
                add({allCode ? DataGuess::Kind::Pointers : DataGuess::Kind::Words, at16, 2 * n,
                     allCode ? "word table read with an index; every entry is an instruction address" : "word table read with an index (low and high byte)", ""});
                a += 2 * n;
                continue;
            }
        }
        // PETSCII text
        unsigned len = 0, letters = 0;
        std::string preview;
        while (isFree(a + len) && petsciiPrintable(mem_.read8(static_cast<Address>(a + len))) && len < 256 &&
               (len == 0 || !refs_.count(static_cast<Address>(a + len)))) {
            const Byte b = mem_.read8(static_cast<Address>(a + len));
            if (petsciiLetterOrDigit(b)) ++letters;
            if (preview.size() < 40) preview += petsciiToText(b);
            ++len;
        }
        const bool terminated = isFree(a + len) && mem_.read8(static_cast<Address>(a + len)) == 0;
        const bool referenced = refs_.count(at16) != 0;
        if ((len >= 4 && letters >= 3) || (referenced && len >= 2 && letters >= 1 && terminated)) {
            const unsigned total = len + (terminated ? 1 : 0);
            add({DataGuess::Kind::Text, at16, total,
                 std::string(referenced ? "referenced, " : "") + std::to_string(len) + " PETSCII characters" + (terminated ? ", zero-terminated" : ""),
                 preview});
            a += total;
            continue;
        }
        ++a;
    }
    std::sort(out.begin(), out.end(), [](const DataGuess& x, const DataGuess& y) { return x.start < y.start; });
    return out;
}

std::vector<RoutineInfo> Analysis::routines() const {
    std::set<Address> entries;
    for (const Address a : callTargets_) if (at(a)) entries.insert(a);
    for (const Address a : entries_) if (at(a)) entries.insert(a);
    for (const Address a : tableTargets_) if (at(a)) entries.insert(a);
    for (const auto& [a, v] : vectorEntries_) if (at(a)) entries.insert(a);

    std::map<Address, RoutineInfo> infos;
    std::map<Address, std::set<Address>> jumpers;  // routine entry -> JMP/branch instructions of other routines
    for (const Address r : entries) {
        RoutineInfo info;
        info.entry = r;
        info.viaTable = tableTargets_.count(r) != 0;
        std::set<Address> seen, callees;
        std::vector<Address> work{r};
        std::set<Address> external;
        while (!work.empty()) {
            const Address a = work.back();
            work.pop_back();
            if (seen.count(a)) continue;
            const Instruction* in = at(a);
            if (!in) continue;
            seen.insert(a);
            auto go = [&](Address t, bool isJump) {
                if (!inRange(t)) {
                    external.insert(t);
                    if (!Kernal::entryName(t).empty()) info.kernalCalls.emplace_back(a, t);
                    return;
                }
                if (t != r && entries.count(t)) {
                    callees.insert(t);
                    if (isJump) jumpers[t].insert(a);
                    return;
                }
                work.push_back(t);
            };
            const auto next = static_cast<Address>(a + in->length());
            const bool nextInRange = inRange(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(in->length()));
            switch (in->flow) {
                case FlowKind::Normal:
                    if (nextInRange) go(next, false);
                    break;
                case FlowKind::CondJump:
                    if (nextInRange) go(next, false);
                    go(*in->target, true);
                    break;
                case FlowKind::Call:
                    if (in->target) {
                        if (inRange(*in->target)) callees.insert(*in->target);
                        else {
                            external.insert(*in->target);
                            if (!Kernal::entryName(*in->target).empty()) info.kernalCalls.emplace_back(a, *in->target);
                        }
                    }
                    if (nextInRange) go(next, false);
                    break;
                case FlowKind::Jump:
                    if (in->mode == Mode::Absolute && in->target) go(*in->target, true);
                    break;
                case FlowKind::Return:
                case FlowKind::Halt: break;
            }
            if (in->flow == FlowKind::Jump || in->mnemonic == "RTS") {  // jump tables used by this instruction
                for (const JumpTable& t : tables_)
                    if (t.jump == a)
                        for (const Address target : t.targets)
                            if (inRange(target) && entries.count(target) && target != r) callees.insert(target);
            }
            // memory operands
            if (hasMemoryOperand(*in) && in->reference && in->flow != FlowKind::Jump) {
                const Address ref = *in->reference;
                if (ref >= 0xD000 && ref < 0xE000) {
                    if (readsMemory(*in)) info.ioReads.insert(ref);
                    if (writesMemory(*in)) info.ioWrites.insert(ref);
                }
                if (ref < 0x100) {
                    info.zeroPage.insert(ref);
                    if (in->mode == Mode::IndirectX || in->mode == Mode::IndirectY) info.zeroPage.insert(static_cast<Address>((ref + 1) & 0xFF));
                }
                if (ref <= 1 && writesMemory(*in) && (in->mode == Mode::ZeroPage || in->mode == Mode::Absolute)) {
                    int value = -1;
                    if (const auto p = previous(a)) {
                        const Instruction& ld = code_.at(*p);
                        const std::string sm = baseMnemonic(*in), lm = baseMnemonic(ld);
                        if (ld.mode == Mode::Immediate && isLoad(lm) && isStore(sm) && loadRegister(lm) == storeRegister(sm)) value = static_cast<int>(ld.value);
                    }
                    info.portWrites.emplace_back(a, value);
                }
            }
            if (in->mode == Mode::Indirect && in->reference && *in->reference < 0x100) {
                info.zeroPage.insert(*in->reference);
                info.zeroPage.insert(static_cast<Address>((*in->reference + 1) & 0xFF));
            }
        }
        // blocks
        Address blockStart = 0, blockEnd = 0;
        bool open = false;
        for (const Address a : seen) {
            const Instruction* in = at(a);
            ++info.instructions;
            info.bytes += static_cast<unsigned>(in->length());
            const auto end = static_cast<Address>(a + in->length());
            if (open && a == blockEnd) {
                blockEnd = end;
            } else {
                if (open) info.blocks.emplace_back(blockStart, blockEnd);
                blockStart = a;
                blockEnd = end;
                open = true;
            }
        }
        if (open) info.blocks.emplace_back(blockStart, blockEnd);
        info.start = info.blocks.empty() ? r : info.blocks.front().first;
        info.end = info.blocks.empty() ? r : info.blocks.back().second;
        info.callees.assign(callees.begin(), callees.end());
        info.externalCalls.assign(external.begin(), external.end());
        infos[r] = std::move(info);
    }
    for (auto& [r, info] : infos) {
        std::set<Address> callers;
        if (const auto it = callers_.find(r); it != callers_.end()) callers.insert(it->second.begin(), it->second.end());
        if (const auto it = jumpers.find(r); it != jumpers.end()) callers.insert(it->second.begin(), it->second.end());
        info.callers.assign(callers.begin(), callers.end());
    }
    // depth over the call graph, cycles cut
    std::map<Address, int> depth;
    std::set<Address> active;
    std::function<int(Address)> depthOf = [&](Address r) -> int {
        if (const auto it = depth.find(r); it != depth.end()) return it->second;
        if (active.count(r)) return 0;
        active.insert(r);
        int d = 0;
        for (const Address c : infos[r].callees)
            if (infos.count(c) && c != r) d = std::max(d, 1 + depthOf(c));
        active.erase(r);
        depth[r] = d;
        return d;
    };
    std::vector<RoutineInfo> out;
    for (auto& [r, info] : infos) {
        info.depth = depthOf(r);
        out.push_back(info);
    }
    return out;
}

}  // namespace m6510
