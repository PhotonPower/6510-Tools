#include "m6510/cpu.hpp"

#include <algorithm>

#include <array>
#include <cstring>

#include "m6510/opcodes.hpp"

namespace m6510 {

namespace {

// Operations, one per mnemonic of the opcode table.
enum class Op : std::uint8_t {
    ADC, AND, ASL, BCC, BCS, BEQ, BIT, BMI, BNE, BPL, BRK, BVC, BVS, CLC, CLD, CLI, CLV, CMP, CPX, CPY, DEC, DEX, DEY,
    EOR, INC, INX, INY, JMP, JSR, LDA, LDX, LDY, LSR, NOP, ORA, PHA, PHP, PLA, PLP, ROL, ROR, RTI, RTS, SBC, SEC, SED,
    SEI, STA, STX, STY, TAX, TAY, TSX, TXA, TXS, TYA,
    // undocumented
    SLO, RLA, SRE, RRA, SAX, LAX, DCP, ISC, ANC, ALR, ARR, SBX, USBC, ANE, LXA, LAS, TAS, SHA, SHX, SHY, JAM,
    Count
};

constexpr const char* kOpNames[] = {
    "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRK", "BVC", "BVS", "CLC", "CLD", "CLI",
    "CLV", "CMP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "JMP", "JSR", "LDA", "LDX", "LDY",
    "LSR", "NOP", "ORA", "PHA", "PHP", "PLA", "PLP", "ROL", "ROR", "RTI", "RTS", "SBC", "SEC", "SED", "SEI", "STA",
    "STX", "STY", "TAX", "TAY", "TSX", "TXA", "TXS", "TYA", "SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP", "ISC",
    "ANC", "ALR", "ARR", "SBX", "USBC", "ANE", "LXA", "LAS", "TAS", "SHA", "SHX", "SHY", "JAM"};
static_assert(sizeof(kOpNames) / sizeof(kOpNames[0]) == static_cast<std::size_t>(Op::Count));

const std::array<Op, 256>& opTable() {
    static const std::array<Op, 256> t = [] {
        std::array<Op, 256> a{};
        for (int code = 0; code < 256; ++code) {
            const char* m = opInfo(static_cast<Byte>(code)).mnemonic;
            for (int k = 0; k < static_cast<int>(Op::Count); ++k) {
                if (std::strcmp(kOpNames[k], m) == 0) a[static_cast<std::size_t>(code)] = static_cast<Op>(k);
            }
        }
        return a;
    }();
    return t;
}

/// "Magic" constant of the unstable ANE/LXA opcodes (the value of Tom Harte's test set, see docs/emulator.md).
constexpr Byte kMagic = 0xEE;

}  // namespace

// ---- execution ----------------------------------------------------------------------------------------
class Exec {
public:
    explicit Exec(Cpu& c) : c_(c), r_(c.r_) {}

    StepResult run() {
        const Address pc = r_.pc;
        const Byte opcode = c_.fetch(pc);
        const OpInfo& info = opInfo(opcode);
        const Op op = opTable()[opcode];
        const bool iBefore = r_.flag(kFlagI);
        cycles_ = info.cycles;
        mode_ = info.mode;

        // operand / effective address
        const Byte b1 = c_.fetch(static_cast<Address>(pc + 1));
        const Byte b2 = c_.fetch(static_cast<Address>(pc + 2));
        const Word word = static_cast<Word>(b1 | (b2 << 8));
        r_.pc = static_cast<Address>(pc + modeLength(info.mode));
        switch (info.mode) {
            case Mode::Implied:
            case Mode::Accumulator:
                break;
            case Mode::Immediate:
                ea_ = static_cast<Address>(pc + 1);
                break;
            case Mode::ZeroPage:
                ea_ = b1;
                break;
            case Mode::ZeroPageX:
                ea_ = static_cast<Byte>(b1 + r_.x);
                break;
            case Mode::ZeroPageY:
                ea_ = static_cast<Byte>(b1 + r_.y);
                break;
            case Mode::Absolute:
                ea_ = word;
                base_ = word;
                break;
            case Mode::AbsoluteX:
                base_ = word;
                ea_ = static_cast<Address>(word + r_.x);
                crossed_ = (ea_ & 0xFF00) != (word & 0xFF00);
                break;
            case Mode::AbsoluteY:
                base_ = word;
                ea_ = static_cast<Address>(word + r_.y);
                crossed_ = (ea_ & 0xFF00) != (word & 0xFF00);
                break;
            case Mode::Indirect: {
                const Byte lo = c_.read(word);
                const Byte hi = c_.read(static_cast<Address>((word & 0xFF00) | ((word + 1) & 0x00FF)));  // JMP ($xxFF)
                ea_ = static_cast<Address>(lo | (hi << 8));
                break;
            }
            case Mode::IndirectX: {
                const auto zp = static_cast<Byte>(b1 + r_.x);
                ea_ = static_cast<Address>(c_.read(zp) | (c_.read(static_cast<Byte>(zp + 1)) << 8));
                break;
            }
            case Mode::IndirectY: {
                base_ = static_cast<Address>(c_.read(b1) | (c_.read(static_cast<Byte>(b1 + 1)) << 8));
                ea_ = static_cast<Address>(base_ + r_.y);
                crossed_ = (ea_ & 0xFF00) != (base_ & 0xFF00);
                break;
            }
            case Mode::Relative:
                ea_ = static_cast<Address>(r_.pc + static_cast<std::int8_t>(b1));
                break;
        }
        if (info.pagePenalty && crossed_ && info.mode != Mode::Relative) ++cycles_;

        execute(op, opcode);

        // CLI, SEI and PLP change the I flag only for the interrupt poll after the next instruction
        c_.irqInhibit_ = (op == Op::CLI || op == Op::SEI || op == Op::PLP) ? iBefore : r_.flag(kFlagI);
        StepResult res;
        res.cycles = cycles_;
        if (c_.halted_) res.reason = StopReason::Halted;
        return res;
    }

private:
    Cpu& c_;
    Registers& r_;
    Address ea_ = 0, base_ = 0;
    bool crossed_ = false;
    unsigned cycles_ = 0;
    Mode mode_ = Mode::Implied;

    void setNZ(Byte v) {
        r_.p = static_cast<Byte>((r_.p & ~(kFlagN | kFlagZ)) | (v & 0x80) | (v == 0 ? kFlagZ : 0));
    }
    void setFlag(Byte f, bool on) { r_.p = static_cast<Byte>(on ? (r_.p | f) : (r_.p & ~f)); }
    [[nodiscard]] bool carry() const { return r_.flag(kFlagC); }

    Byte load() { return c_.read(ea_); }
    // read-modify-write: read, dummy write of the old value, write of the new value
    template <class F>
    Byte modify(F f) {
        if (mode_ == Mode::Accumulator) {
            r_.a = f(r_.a);
            return r_.a;
        }
        const Byte old = c_.read(ea_);
        c_.write(ea_, old);
        const Byte v = f(old);
        c_.write(ea_, v);
        return v;
    }

    Byte asl(Byte v) {
        setFlag(kFlagC, (v & 0x80) != 0);
        v = static_cast<Byte>(v << 1);
        setNZ(v);
        return v;
    }
    Byte lsr(Byte v) {
        setFlag(kFlagC, (v & 0x01) != 0);
        v = static_cast<Byte>(v >> 1);
        setNZ(v);
        return v;
    }
    Byte rol(Byte v) {
        const bool c = carry();
        setFlag(kFlagC, (v & 0x80) != 0);
        v = static_cast<Byte>((v << 1) | (c ? 1 : 0));
        setNZ(v);
        return v;
    }
    Byte ror(Byte v) {
        const bool c = carry();
        setFlag(kFlagC, (v & 0x01) != 0);
        v = static_cast<Byte>((v >> 1) | (c ? 0x80 : 0));
        setNZ(v);
        return v;
    }

    void adc(Byte v) {
        const unsigned a = r_.a, c = carry() ? 1 : 0;
        if (r_.flag(kFlagD)) {
            // NMOS decimal mode: Z from the binary sum, N and V from the intermediate result
            unsigned t = (a & 0x0F) + (v & 0x0F) + c;
            if (t > 0x09) t += 0x06;
            t = (t <= 0x0F) ? (t & 0x0F) + (a & 0xF0) + (v & 0xF0) : (t & 0x0F) + (a & 0xF0) + (v & 0xF0) + 0x10;
            setFlag(kFlagZ, ((a + v + c) & 0xFF) == 0);
            setFlag(kFlagN, (t & 0x80) != 0);
            setFlag(kFlagV, ((a ^ t) & 0x80) != 0 && ((a ^ v) & 0x80) == 0);
            if ((t & 0x1F0) > 0x90) t += 0x60;
            setFlag(kFlagC, (t & 0xFF0) > 0xF0);
            r_.a = static_cast<Byte>(t);
        } else {
            const unsigned sum = a + v + c;
            setFlag(kFlagC, sum > 0xFF);
            setFlag(kFlagV, (~(a ^ v) & (a ^ sum) & 0x80) != 0);
            r_.a = static_cast<Byte>(sum);
            setNZ(r_.a);
        }
    }
    void sbc(Byte v) {
        const unsigned a = r_.a, borrow = carry() ? 0 : 1;
        const unsigned t = a - v - borrow;
        if (r_.flag(kFlagD)) {
            // NMOS decimal mode: all flags from the binary difference
            unsigned lo = (a & 0x0F) - (v & 0x0F) - borrow;
            unsigned res;
            if (lo & 0x10) res = ((lo - 6) & 0x0F) | ((a & 0xF0) - (v & 0xF0) - 0x10);
            else res = (lo & 0x0F) | ((a & 0xF0) - (v & 0xF0));
            if (res & 0x100) res -= 0x60;
            setFlag(kFlagC, t < 0x100);
            setNZ(static_cast<Byte>(t));
            setFlag(kFlagV, ((a ^ t) & 0x80) != 0 && ((a ^ v) & 0x80) != 0);
            r_.a = static_cast<Byte>(res);
        } else {
            setFlag(kFlagC, t < 0x100);
            setFlag(kFlagV, ((a ^ t) & 0x80) != 0 && ((a ^ v) & 0x80) != 0);
            r_.a = static_cast<Byte>(t);
            setNZ(r_.a);
        }
    }
    void compare(Byte reg, Byte v) {
        setFlag(kFlagC, reg >= v);
        setNZ(static_cast<Byte>(reg - v));
    }
    void branch(bool take) {
        if (!take) return;
        ++cycles_;
        const Address next = r_.pc;
        if ((next & 0xFF00) != (ea_ & 0xFF00)) ++cycles_;
        r_.pc = ea_;
    }
    // SHA/SHX/SHY/TAS: value & (high byte of the base address + 1); a page crossing replaces the high byte of the address
    void storeHigh(Byte v) {
        const auto value = static_cast<Byte>(v & ((base_ >> 8) + 1));
        Address a = ea_;
        if (crossed_) a = static_cast<Address>((value << 8) | (a & 0xFF));
        c_.write(a, value);
    }

    void execute(Op op, Byte opcode) {
        switch (op) {
            // ---- loads, stores, transfers
            case Op::LDA: r_.a = load(); setNZ(r_.a); break;
            case Op::LDX: r_.x = load(); setNZ(r_.x); break;
            case Op::LDY: r_.y = load(); setNZ(r_.y); break;
            case Op::STA: c_.write(ea_, r_.a); break;
            case Op::STX: c_.write(ea_, r_.x); break;
            case Op::STY: c_.write(ea_, r_.y); break;
            case Op::TAX: r_.x = r_.a; setNZ(r_.x); break;
            case Op::TAY: r_.y = r_.a; setNZ(r_.y); break;
            case Op::TXA: r_.a = r_.x; setNZ(r_.a); break;
            case Op::TYA: r_.a = r_.y; setNZ(r_.a); break;
            case Op::TSX: r_.x = r_.s; setNZ(r_.x); break;
            case Op::TXS: r_.s = r_.x; break;
            // ---- stack
            case Op::PHA: c_.push(r_.a); break;
            case Op::PHP: c_.push(static_cast<Byte>(r_.p | kFlagB | kFlagU)); break;
            case Op::PLA: r_.a = c_.pull(); setNZ(r_.a); break;
            case Op::PLP: r_.p = static_cast<Byte>((c_.pull() & ~kFlagB) | kFlagU); break;
            // ---- arithmetic and logic
            case Op::ORA: r_.a = static_cast<Byte>(r_.a | load()); setNZ(r_.a); break;
            case Op::AND: r_.a = static_cast<Byte>(r_.a & load()); setNZ(r_.a); break;
            case Op::EOR: r_.a = static_cast<Byte>(r_.a ^ load()); setNZ(r_.a); break;
            case Op::ADC: adc(load()); break;
            case Op::SBC:
            case Op::USBC: sbc(load()); break;
            case Op::CMP: compare(r_.a, load()); break;
            case Op::CPX: compare(r_.x, load()); break;
            case Op::CPY: compare(r_.y, load()); break;
            case Op::BIT: {
                const Byte v = load();
                setFlag(kFlagZ, (r_.a & v) == 0);
                r_.p = static_cast<Byte>((r_.p & ~(kFlagN | kFlagV)) | (v & (kFlagN | kFlagV)));
                break;
            }
            case Op::INC: modify([this](Byte v) { v = static_cast<Byte>(v + 1); setNZ(v); return v; }); break;
            case Op::DEC: modify([this](Byte v) { v = static_cast<Byte>(v - 1); setNZ(v); return v; }); break;
            case Op::INX: setNZ(++r_.x); break;
            case Op::INY: setNZ(++r_.y); break;
            case Op::DEX: setNZ(--r_.x); break;
            case Op::DEY: setNZ(--r_.y); break;
            case Op::ASL: modify([this](Byte v) { return asl(v); }); break;
            case Op::LSR: modify([this](Byte v) { return lsr(v); }); break;
            case Op::ROL: modify([this](Byte v) { return rol(v); }); break;
            case Op::ROR: modify([this](Byte v) { return ror(v); }); break;
            // ---- flags
            case Op::CLC: setFlag(kFlagC, false); break;
            case Op::SEC: setFlag(kFlagC, true); break;
            case Op::CLI: setFlag(kFlagI, false); break;
            case Op::SEI: setFlag(kFlagI, true); break;
            case Op::CLV: setFlag(kFlagV, false); break;
            case Op::CLD: setFlag(kFlagD, false); break;
            case Op::SED: setFlag(kFlagD, true); break;
            // ---- control flow
            case Op::BPL: branch(!r_.flag(kFlagN)); break;
            case Op::BMI: branch(r_.flag(kFlagN)); break;
            case Op::BVC: branch(!r_.flag(kFlagV)); break;
            case Op::BVS: branch(r_.flag(kFlagV)); break;
            case Op::BCC: branch(!r_.flag(kFlagC)); break;
            case Op::BCS: branch(r_.flag(kFlagC)); break;
            case Op::BNE: branch(!r_.flag(kFlagZ)); break;
            case Op::BEQ: branch(r_.flag(kFlagZ)); break;
            case Op::JMP: r_.pc = ea_; break;
            case Op::JSR: {
                const auto ret = static_cast<Address>(r_.pc - 1);  // address of the JSR's last byte
                c_.push(static_cast<Byte>(ret >> 8));
                c_.push(static_cast<Byte>(ret));
                // the high byte of the target is fetched after the pushes (matters if they overwrite it)
                r_.pc = static_cast<Address>((ea_ & 0x00FF) | (c_.fetch(ret) << 8));
                ++c_.depth_;
                break;
            }
            case Op::RTS: {
                const Byte lo = c_.pull();
                const Byte hi = c_.pull();
                r_.pc = static_cast<Address>((lo | (hi << 8)) + 1);
                --c_.depth_;
                break;
            }
            case Op::RTI: {
                r_.p = static_cast<Byte>((c_.pull() & ~kFlagB) | kFlagU);
                const Byte lo = c_.pull();
                const Byte hi = c_.pull();
                r_.pc = static_cast<Address>(lo | (hi << 8));
                --c_.depth_;
                break;
            }
            case Op::BRK:
                r_.pc = static_cast<Address>(r_.pc + 1);  // the byte after BRK is skipped
                c_.interruptSequence(kIrqVector, true);
                break;
            case Op::NOP:
                if (mode_ != Mode::Implied && mode_ != Mode::Immediate) (void)load();  // the NOP variants read their operand
                break;
            // ---- undocumented, stable
            case Op::SLO: { const Byte v = modify([this](Byte x) { return asl(x); }); r_.a = static_cast<Byte>(r_.a | v); setNZ(r_.a); break; }
            case Op::RLA: { const Byte v = modify([this](Byte x) { return rol(x); }); r_.a = static_cast<Byte>(r_.a & v); setNZ(r_.a); break; }
            case Op::SRE: { const Byte v = modify([this](Byte x) { return lsr(x); }); r_.a = static_cast<Byte>(r_.a ^ v); setNZ(r_.a); break; }
            case Op::RRA: { const Byte v = modify([this](Byte x) { return ror(x); }); adc(v); break; }
            case Op::DCP: { const Byte v = modify([](Byte x) { return static_cast<Byte>(x - 1); }); compare(r_.a, v); break; }
            case Op::ISC: { const Byte v = modify([](Byte x) { return static_cast<Byte>(x + 1); }); sbc(v); break; }
            case Op::SAX: c_.write(ea_, static_cast<Byte>(r_.a & r_.x)); break;
            case Op::LAX: r_.a = r_.x = load(); setNZ(r_.a); break;
            case Op::ANC:
                r_.a = static_cast<Byte>(r_.a & load());
                setNZ(r_.a);
                setFlag(kFlagC, (r_.a & 0x80) != 0);
                break;
            case Op::ALR:
                r_.a = lsr(static_cast<Byte>(r_.a & load()));
                break;
            case Op::ARR: {
                const auto t = static_cast<Byte>(r_.a & load());
                const bool c = carry();
                if (r_.flag(kFlagD)) {
                    unsigned res = static_cast<unsigned>((t >> 1) | (c ? 0x80 : 0));
                    setFlag(kFlagN, c);
                    setFlag(kFlagZ, res == 0);
                    setFlag(kFlagV, ((t ^ res) & 0x40) != 0);
                    if (((t & 0x0F) + (t & 0x01)) > 0x05) res = (res & 0xF0) | ((res + 0x06) & 0x0F);
                    if (((t & 0xF0) + (t & 0x10)) > 0x50) {
                        res = (res + 0x60) & 0xFF;
                        setFlag(kFlagC, true);
                    } else {
                        setFlag(kFlagC, false);
                    }
                    r_.a = static_cast<Byte>(res);
                } else {
                    r_.a = static_cast<Byte>((t >> 1) | (c ? 0x80 : 0));
                    setNZ(r_.a);
                    setFlag(kFlagC, (r_.a & 0x40) != 0);
                    setFlag(kFlagV, (((r_.a >> 6) ^ (r_.a >> 5)) & 1) != 0);
                }
                break;
            }
            case Op::SBX: {
                const unsigned t = static_cast<unsigned>(r_.a & r_.x) - load();
                setFlag(kFlagC, t < 0x100);
                r_.x = static_cast<Byte>(t);
                setNZ(r_.x);
                break;
            }
            // ---- undocumented, unstable (behaviour of Tom Harte's test set)
            case Op::ANE: r_.a = static_cast<Byte>((r_.a | kMagic) & r_.x & load()); setNZ(r_.a); break;
            case Op::LXA: r_.a = r_.x = static_cast<Byte>((r_.a | kMagic) & load()); setNZ(r_.a); break;
            case Op::LAS: r_.a = r_.x = r_.s = static_cast<Byte>(load() & r_.s); setNZ(r_.a); break;
            case Op::TAS: r_.s = static_cast<Byte>(r_.a & r_.x); storeHigh(r_.s); break;
            case Op::SHA: storeHigh(static_cast<Byte>(r_.a & r_.x)); break;
            case Op::SHX: storeHigh(r_.x); break;
            case Op::SHY: storeHigh(r_.y); break;
            case Op::JAM:
                r_.pc = static_cast<Address>(r_.pc - 1);  // stays on the opcode
                c_.halted_ = true;
                break;
            case Op::Count:
                break;
        }
        (void)opcode;
    }
};

// ---- Cpu ----------------------------------------------------------------------------------------------
Cpu::Cpu(Memory& memory) : mem_(memory) {}

void Cpu::powerOn() {
    r_ = Registers{};
    r_.s = 0x00;
    r_.p = kFlagU;
    instructions_ = cycles_ = interrupts_ = 0;
    reset();
}

void Cpu::reset() {
    halted_ = false;
    stopRequested_ = false;
    depth_ = 0;
    nmiPending_ = false;
    irqInhibit_ = true;
    portDir_ = 0;
    portData_ = 0;
    if (portHook) portHook(portDir_, portData_);
    r_.p = static_cast<Byte>(r_.p | kFlagI | kFlagU);
    r_.s = static_cast<Byte>(r_.s - 3);
    r_.pc = static_cast<Address>(fetch(kResetVector) | (fetch(kResetVector + 1) << 8));
    cycles_ += 7;
}

Byte Cpu::portValue() const {
    return static_cast<Byte>((portData_ & portDir_) | (portInputs_ & ~portDir_));
}

Byte Cpu::fetch(Address a) const {
    if (portEnabled_ && a < 2) return a == 0 ? portDir_ : portValue();
    Byte v = 0;
    if (readOverride && readOverride(a, v)) return v;
    return mem_.read8(a);
}

Byte Cpu::peek(Address a) const { return fetch(a); }

Byte Cpu::read(Address a) {
    const Byte v = fetch(a);
    if (accessHook) accessHook(a, v, false);
    for (const Watch& w : watches_) {
        if (w.onRead && static_cast<Address>(a - w.start) < w.length) {
            watchHit_ = true;
            watchAddress_ = a;
        }
    }
    return v;
}

void Cpu::write(Address a, Byte v) {
    if (tracking_) written_[a] = 1;
    if (accessHook) accessHook(a, v, true);
    for (const Watch& w : watches_) {
        if (w.onWrite && static_cast<Address>(a - w.start) < w.length) {
            watchHit_ = true;
            watchAddress_ = a;
        }
    }
    if (portEnabled_ && a < 2) {
        if (a == 0) portDir_ = v;
        else portData_ = v;
        mem_.write8(a, v);  // the RAM underneath is written as well (docs/emulator.md)
        if (portHook) portHook(portDir_, portData_);
        return;
    }
    if (writeOverride && writeOverride(a, v)) return;
    mem_.write8(a, v);
}

void Cpu::push(Byte v) {
    write(static_cast<Address>(kStackPage | r_.s), v);
    r_.s = static_cast<Byte>(r_.s - 1);
}

Byte Cpu::pull() {
    r_.s = static_cast<Byte>(r_.s + 1);
    return read(static_cast<Address>(kStackPage | r_.s));
}

void Cpu::interruptSequence(Address vector, bool brk) {
    push(static_cast<Byte>(r_.pc >> 8));
    push(static_cast<Byte>(r_.pc));
    push(static_cast<Byte>((r_.p | kFlagU | (brk ? kFlagB : 0)) & ~(brk ? 0 : kFlagB)));
    r_.p = static_cast<Byte>(r_.p | kFlagI);
    r_.pc = static_cast<Address>(fetch(vector) | (fetch(static_cast<Address>(vector + 1)) << 8));
    ++depth_;
    ++interrupts_;
}

void Cpu::returnFromSubroutine() {
    const Byte lo = pull();
    const Byte hi = pull();
    r_.pc = static_cast<Address>((lo | (hi << 8)) + 1);
    --depth_;
}

void Cpu::addWatchpoint(Address start, unsigned length, bool onRead, bool onWrite) {
    if (length == 0) length = 1;
    watches_.push_back({start, length, onRead, onWrite});
}

StepResult Cpu::step() {
    StepResult res;
    if (halted_) {
        res.reason = StopReason::Halted;
        return res;
    }
    extraCycles_ = 0;
    if (nmiPending_ || (irqLine_ && !irqInhibit_)) {
        const bool nmi = nmiPending_;
        nmiPending_ = false;
        startPc_ = r_.pc;
        interruptSequence(nmi ? kNmiVector : kIrqVector, false);
        irqInhibit_ = true;
        res.cycles = 7;
        res.interrupt = true;
        cycles_ += 7;
        return res;
    }
    startPc_ = r_.pc;
    if (executeHook && r_.pc >= hookLow && r_.pc <= hookHigh && executeHook(*this)) {
        ++instructions_;
        res.cycles = extraCycles_;
        cycles_ += extraCycles_;
        irqInhibit_ = r_.flag(kFlagI);
        return res;
    }
    if (traceHook) traceHook(r_, r_.pc, fetch(r_.pc));
    if (trackingExec_) executed_[r_.pc] = 1;
    Exec exec(*this);
    res = exec.run();
    res.cycles += extraCycles_;
    ++instructions_;
    cycles_ += res.cycles;
    return res;
}

RunResult Cpu::runLoop(std::uint64_t maxInstructions, std::optional<Address> until, bool untilReturn,
                       std::optional<Byte> untilStack) {
    RunResult res;
    const int startDepth = depth_;
    stopRequested_ = false;
    for (std::uint64_t n = 0;; ++n) {
        if (halted_) {
            res.reason = StopReason::Halted;
            return res;
        }
        if (until && r_.pc == *until && (!untilStack || r_.s == *untilStack)) {
            res.reason = StopReason::Reached;
            return res;
        }
        if (watchWritten_ && inWrittenCode(r_.pc)) {
            res.reason = StopReason::WrittenCode;
            return res;
        }
        if (n > 0 && breakpoints_.count(r_.pc)) {
            res.reason = StopReason::Breakpoint;
            return res;
        }
        if (n >= maxInstructions) {
            res.reason = StopReason::Limit;
            return res;
        }
        watchHit_ = false;
        const StepResult s = step();
        ++res.instructions;
        res.cycles += s.cycles;
        if (stopRequested_) {
            stopRequested_ = false;
            res.reason = StopReason::Requested;
            return res;
        }
        if (watchHit_) {
            res.reason = StopReason::Watchpoint;
            res.watchAddress = watchAddress_;
            return res;
        }
        if (untilReturn && depth_ < startDepth) {
            res.reason = StopReason::Returned;
            return res;
        }
        if (s.reason == StopReason::Halted) {
            res.reason = StopReason::Halted;
            return res;
        }
    }
}

RunResult Cpu::run(std::uint64_t maxInstructions) { return runLoop(maxInstructions, std::nullopt, false); }

RunResult Cpu::runUntil(Address address, std::uint64_t maxInstructions) {
    // the instruction at the start runs even if pc is already there
    if (r_.pc == address && maxInstructions > 0) {
        RunResult first;
        watchHit_ = false;
        const StepResult s = step();
        first.instructions = 1;
        first.cycles = s.cycles;
        if (s.reason == StopReason::Halted) {
            first.reason = StopReason::Halted;
            return first;
        }
        RunResult rest = runLoop(maxInstructions - 1, address, false);
        rest.instructions += 1;
        rest.cycles += first.cycles;
        return rest;
    }
    return runLoop(maxInstructions, address, false);
}

RunResult Cpu::runUntilReturn(std::uint64_t maxInstructions) { return runLoop(maxInstructions, std::nullopt, true); }

RunResult Cpu::stepOver(std::uint64_t maxInstructions) {
    if (!halted_ && fetch(r_.pc) == 0x20) {  // JSR: run until it comes back with the same stack pointer
        const auto ret = static_cast<Address>(r_.pc + 3);
        const Byte s = r_.s;
        watchHit_ = false;
        const StepResult first = step();
        RunResult res = runLoop(maxInstructions > 0 ? maxInstructions - 1 : 0, ret, false, s);
        res.instructions += 1;
        res.cycles += first.cycles;
        if (res.reason == StopReason::Reached) res.reason = StopReason::None;
        return res;
    }
    RunResult res;
    if (maxInstructions == 0) {
        res.reason = StopReason::Limit;
        return res;
    }
    watchHit_ = false;
    stopRequested_ = false;
    const StepResult s = step();
    res.instructions = 1;
    res.cycles = s.cycles;
    res.reason = s.reason;
    if (stopRequested_) {
        stopRequested_ = false;
        res.reason = StopReason::Requested;
    } else if (watchHit_) {
        res.reason = StopReason::Watchpoint;
        res.watchAddress = watchAddress_;
    }
    return res;
}

RunResult Cpu::call(Address entry, Address returnAddress, std::uint64_t maxInstructions) {
    const Byte s0 = r_.s;
    const auto ret = static_cast<Address>(returnAddress - 1);
    push(static_cast<Byte>(ret >> 8));
    push(static_cast<Byte>(ret));
    r_.pc = entry;
    halted_ = false;
    ++depth_;
    RunResult res = runLoop(maxInstructions, returnAddress, false);
    if (res.reason == StopReason::Reached) {
        if (r_.s == s0) res.reason = StopReason::Returned;
        else --depth_;  // reached the address on another stack level: not a return (see docs)
    }
    return res;
}

RunResult Cpu::callInterrupt(Address entry, Address returnAddress, bool kernalFrame, std::uint64_t maxInstructions) {
    const Byte s0 = r_.s;
    push(static_cast<Byte>(returnAddress >> 8));
    push(static_cast<Byte>(returnAddress));
    push(static_cast<Byte>((r_.p | kFlagU) & ~kFlagB));
    if (kernalFrame) {
        push(r_.a);
        push(r_.x);
        push(r_.y);
    }
    r_.pc = entry;
    halted_ = false;
    ++depth_;
    RunResult res = runLoop(maxInstructions, returnAddress, false);
    if (res.reason == StopReason::Reached) {
        if (r_.s == s0) res.reason = StopReason::Returned;
        else --depth_;
    }
    return res;
}

void Cpu::trackExecution(bool on) {
    trackingExec_ = on;
    if (on) executed_.assign(0x10000, 0);
    else executed_.clear();
}

std::vector<Address> Cpu::executedAddresses() const {
    std::vector<Address> out;
    for (std::size_t a = 0; a < executed_.size(); ++a)
        if (executed_[a]) out.push_back(static_cast<Address>(a));
    return out;
}

void Cpu::trackWrites(bool on) {
    tracking_ = on;
    if (on) written_.assign(0x10000, 0);
    else {
        written_.clear();
        watchWritten_ = false;
    }
}

std::size_t Cpu::writtenBytes() const {
    std::size_t n = 0;
    for (const std::uint8_t w : written_) n += w;
    return n;
}

std::vector<std::pair<Address, std::uint32_t>> Cpu::writtenRanges(std::size_t minLength) const {
    std::vector<std::pair<Address, std::uint32_t>> out;
    if (!tracking_) return out;
    std::uint32_t a = 0;
    while (a < 0x10000) {
        if (!written_[a]) {
            ++a;
            continue;
        }
        std::uint32_t e = a;
        while (e < 0x10000 && written_[e]) ++e;
        if (e - a >= minLength) out.emplace_back(static_cast<Address>(a), e);
        a = e;
    }
    return out;
}

void Cpu::watchWrittenCode(bool on, std::size_t minLength, std::vector<std::pair<Address, std::uint32_t>> ignore) {
    watchWritten_ = on && tracking_;
    watchMin_ = std::max<std::size_t>(1, minLength);
    watchIgnore_ = std::move(ignore);
}

bool Cpu::inWrittenCode(Address pc) const {
    if (!written_[pc]) return false;
    for (const auto& [first, end] : watchIgnore_)
        if (pc >= first && pc < end) return false;
    // is the written range around pc at least watchMin_ bytes long? (scans at most that far in each direction)
    std::size_t n = 1;
    for (std::uint32_t a = pc; a > 0 && written_[a - 1] && n < watchMin_; --a) ++n;
    for (std::uint32_t a = pc + 1u; a < 0x10000 && written_[a] && n < watchMin_; ++a) ++n;
    return n >= watchMin_;
}

CpuState Cpu::state() const {
    CpuState s;
    s.regs = r_;
    s.halted = halted_;
    s.instructions = instructions_;
    s.cycles = cycles_;
    s.interrupts = interrupts_;
    s.depth = depth_;
    s.irqLine = irqLine_;
    s.nmiPending = nmiPending_;
    s.irqInhibit = irqInhibit_;
    s.portEnabled = portEnabled_;
    s.portDirection = portDir_;
    s.portData = portData_;
    return s;
}

void Cpu::setState(const CpuState& s) {
    r_ = s.regs;
    r_.p = static_cast<Byte>(r_.p | kFlagU);
    halted_ = s.halted;
    instructions_ = s.instructions;
    cycles_ = s.cycles;
    interrupts_ = s.interrupts;
    depth_ = s.depth;
    irqLine_ = s.irqLine;
    nmiPending_ = s.nmiPending;
    irqInhibit_ = s.irqInhibit;
    portEnabled_ = s.portEnabled;
    portDir_ = s.portDirection;
    portData_ = s.portData;
    if (portHook) portHook(portDir_, portData_);
}

}  // namespace m6510
