#include "m6510/c64.hpp"

#include <cctype>
#include <stdexcept>

namespace m6510 {

namespace {

// Register names after "Mapping the Commodore 64" (docs/c64.md).
constexpr const char* kVicNames[0x2F] = {
    "SP0X", "SP0Y", "SP1X", "SP1Y", "SP2X", "SP2Y", "SP3X", "SP3Y", "SP4X", "SP4Y", "SP5X", "SP5Y",
    "SP6X", "SP6Y", "SP7X", "SP7Y", "MSIGX", "SCROLY", "RASTER", "LPENX", "LPENY", "SPENA", "SCROLX", "YXPAND",
    "VMCSB", "VICIRQ", "IRQMASK", "SPBGPR", "SPMC", "XXPAND", "SPSPCL", "SPBGCL", "EXTCOL", "BGCOL0", "BGCOL1", "BGCOL2",
    "BGCOL3", "SPMC0", "SPMC1", "SP0COL", "SP1COL", "SP2COL", "SP3COL", "SP4COL", "SP5COL", "SP6COL", "SP7COL"};
constexpr const char* kSidNames[0x1D] = {
    "FRELO1", "FREHI1", "PWLO1", "PWHI1", "VCREG1", "ATDCY1", "SUREL1", "FRELO2", "FREHI2", "PWLO2",
    "PWHI2",  "VCREG2", "ATDCY2", "SUREL2", "FRELO3", "FREHI3", "PWLO3", "PWHI3", "VCREG3", "ATDCY3",
    "SUREL3", "CUTLO",  "CUTHI",  "RESON",  "SIGVOL", "POTX",   "POTY",  "RANDOM", "ENV3"};
constexpr const char* kCia1Names[16] = {"CIAPRA", "CIAPRB", "CIDDRA", "CIDDRB", "TIMALO", "TIMAHI", "TIMBLO", "TIMBHI",
                                        "TODTEN", "TODSEC", "TODMIN", "TODHRS", "CIASDR", "CIAICR", "CIACRA", "CIACRB"};
constexpr const char* kCia2Names[16] = {"CI2PRA", "CI2PRB", "C2DDRA", "C2DDRB", "TI2ALO", "TI2AHI", "TI2BLO", "TI2BHI",
                                        "TO2TEN", "TO2SEC", "TO2MIN", "TO2HRS", "CI2SDR", "CI2ICR", "CI2CRA", "CI2CRB"};

}  // namespace

C64::C64(Memory& ram, Cpu& cpu) : ram_(ram), cpu_(cpu) {
    cpu_.readOverride = [this](Address a, Byte& v) { return readMapped(a, v, true); };
    cpu_.writeOverride = [this](Address a, Byte v) {
        if (a >= 0xD000 && a < 0xE000 && (ioVisible() || ultimax())) return writeIo(a, v);
        if (ultimax() && ((a >= 0x1000 && a < 0x8000) || (a >= 0xA000 && a < 0xD000))) return true;  // not connected
        return false;  // RAM, also underneath ROM
    };
    cpu_.portHook = [this](Byte, Byte) { config_ = static_cast<Byte>(cpu_.portValue() & 7); };
    cpu_.traceHook = [this](const Registers&, Address, Byte) { tick(); };
    config_ = static_cast<Byte>(cpu_.portValue() & 7);
}

C64::~C64() {
    cpu_.readOverride = nullptr;
    cpu_.writeOverride = nullptr;
    cpu_.portHook = nullptr;
    cpu_.traceHook = nullptr;
}

void C64::loadRom(const std::string& which, const std::vector<Byte>& image) {
    if (which == "basic" || which == "kernal") {
        if (image.size() != 0x2000) throw std::invalid_argument(which + " ROM must be 8192 bytes, got " + std::to_string(image.size()));
        auto& dst = which == "basic" ? basic_ : kernal_;
        std::copy(image.begin(), image.end(), dst.begin());
        (which == "basic" ? hasBasic_ : hasKernal_) = true;
    } else if (which == "char") {
        if (image.size() != 0x1000) throw std::invalid_argument("char ROM must be 4096 bytes, got " + std::to_string(image.size()));
        std::copy(image.begin(), image.end(), char_.begin());
        hasChar_ = true;
    } else {
        throw std::invalid_argument("unknown ROM '" + which + "' (basic, kernal or char)");
    }
}

bool C64::readMapped(Address a, Byte& v, bool effects) {
    const bool loram = (config_ & 1) != 0, hiram = (config_ & 2) != 0;
    auto rom = [](const std::vector<Byte>& r, Address x) { return r.empty() ? Byte{0xFF} : r[(x & 0x1FFF) % r.size()]; };
    if (ultimax()) {
        if (a >= 0x8000 && a < 0xA000) { v = rom(roml_, a); return true; }
        if (a >= 0xE000) { v = rom(romh_, a); return true; }
        if (a >= 0xD000 && a < 0xE000) return readIo(a, v, effects);
        if ((a >= 0x1000 && a < 0x8000) || (a >= 0xA000 && a < 0xD000)) { v = 0xFF; return true; }  // not connected (simplified)
        return false;
    }
    if (cartExrom_ && a >= 0x8000 && a < 0xA000 && loram && hiram) {
        v = rom(roml_, a);
        return true;
    }
    if (a >= 0xA000 && a < 0xC000) {
        if (cartExrom_ && cartGame_) {
            if (!hiram) return false;
            v = rom(romh_, a);
            return true;
        }
        if (!basicVisible()) return false;
        v = basic_[a - 0xA000];
        return true;
    }
    if (a >= 0xD000 && a < 0xE000) {
        if (ioVisible()) return readIo(a, v, effects);
        if (!charVisible()) return false;
        v = char_[a - 0xD000];
        return true;
    }
    if (a >= 0xE000 && kernalVisible()) {
        v = kernal_[a - 0xE000];
        return true;
    }
    return false;
}

void C64::setJoystick(unsigned port, Byte pressed) {
    if (port != 1 && port != 2) throw std::invalid_argument("joystick port must be 1 or 2");
    (port == 1 ? joy1_ : joy2_) = static_cast<Byte>(pressed & 0x1F);
}

void C64::setKey(unsigned column, unsigned row, bool pressed) {
    if (column > 7 || row > 7) throw std::invalid_argument("key matrix position out of range");
    if (pressed) keys_[column] = static_cast<Byte>(keys_[column] | (1u << row));
    else keys_[column] = static_cast<Byte>(keys_[column] & ~(1u << row));
}

namespace {
// The C64 keyboard matrix: [column][row]
const char* const kKeyNames[8][8] = {
    {"DEL", "RETURN", "CRSRRIGHT", "F7", "F1", "F3", "F5", "CRSRDOWN"},
    {"3", "W", "A", "4", "Z", "S", "E", "LSHIFT"},
    {"5", "R", "D", "6", "C", "F", "T", "X"},
    {"7", "Y", "G", "8", "B", "H", "U", "V"},
    {"9", "I", "J", "0", "M", "K", "O", "N"},
    {"+", "P", "L", "-", ".", ":", "@", ","},
    {"POUND", "*", ";", "HOME", "RSHIFT", "=", "UPARROW", "/"},
    {"1", "LEFTARROW", "CTRL", "2", "SPACE", "CBM", "Q", "RUNSTOP"}};
}  // namespace

bool C64::keyPosition(const std::string& name, unsigned& column, unsigned& row) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (unsigned c = 0; c < 8; ++c)
        for (unsigned r = 0; r < 8; ++r)
            if (n == kKeyNames[c][r]) {
                column = c;
                row = r;
                return true;
            }
    return false;
}

std::vector<std::string> C64::pressedKeys() const {
    std::vector<std::string> out;
    for (unsigned c = 0; c < 8; ++c)
        for (unsigned r = 0; r < 8; ++r)
            if ((keys_[c] >> r) & 1) out.emplace_back(kKeyNames[c][r]);
    return out;
}

void C64::insertCartridge(std::vector<Byte> roml, std::vector<Byte> romh, bool exrom, bool game) {
    if (roml.size() > 0x2000 || romh.size() > 0x2000) throw std::invalid_argument("a cartridge ROM is at most 8 KiB");
    roml_ = std::move(roml);
    romh_ = std::move(romh);
    cartExrom_ = exrom;
    cartGame_ = game;
}

void C64::removeCartridge() {
    roml_.clear();
    romh_.clear();
    cartExrom_ = cartGame_ = false;
}

std::string C64::cartridgeMode() const {
    if (cartExrom_ && !cartGame_) return "8K";
    if (cartExrom_ && cartGame_) return "16K";
    if (cartGame_) return "Ultimax";
    return "none";
}

std::string C64::describeConfig() const {
    const bool loram = (config_ & 1) != 0, hiram = (config_ & 2) != 0;
    std::string out;
    if (ultimax()) out = "$8000:ROML ";
    else if (cartExrom_ && loram && hiram) out = "$8000:ROML ";
    std::string a = basicVisible() ? "BASIC" : (cartExrom_ && cartGame_ && hiram) ? "ROMH" : ultimax() ? "-" : "RAM";
    std::string d = ultimax() ? "I/O" : ioVisible() ? "I/O" : charVisible() ? "CHAR" : "RAM";
    std::string e = ultimax() ? "ROMH" : kernalVisible() ? "KERNAL" : "RAM";
    return out + "$A000:" + a + " $D000:" + d + " $E000:" + e;
}

void C64::write(Address a, Byte v) {
    if (cpu_.portEnabled() && a < 2) {
        CpuState st = cpu_.state();
        (a == 0 ? st.portDirection : st.portData) = v;
        cpu_.setState(st);
        ram_.write8(a, v);
        return;
    }
    if (a >= 0xD000 && a < 0xE000 && (ioVisible() || ultimax())) {
        writeIo(a, v);
        return;
    }
    if (ultimax() && ((a >= 0x1000 && a < 0x8000) || (a >= 0xA000 && a < 0xD000))) return;  // not connected
    ram_.write8(a, v);
}

unsigned C64::rasterLine() const {
    return static_cast<unsigned>((cpu_.cycleCount() / kCyclesPerLine) % kRasterLines);
}

Address C64::vicBank() const {
    const Byte pra = static_cast<Byte>(s_.cia2[0] | ~s_.cia2[2]);  // inputs read high
    return static_cast<Address>((3 - (pra & 3)) * 0x4000);
}

Address C64::screenAddress() const {
    return static_cast<Address>(vicBank() + ((vic_[0x18] >> 4) & 0x0F) * 0x400);
}

unsigned C64::timerValue(unsigned index) const {
    const State::Timer& t = s_.timers[index];
    if (!(control(index) & 1) || !countsCycles(index)) return t.counter;
    const std::uint64_t elapsed = cpu_.cycleCount() - t.start;
    if (elapsed <= t.counter) return static_cast<unsigned>(t.counter - elapsed);
    const std::uint64_t rem = (elapsed - t.counter - 1) % (std::uint64_t{t.latch} + 1);
    return static_cast<unsigned>(t.latch - rem);
}

void C64::timerUnderflows(unsigned index, std::uint64_t n) {
    if (n == 0) return;
    Byte& icr = index < 2 ? s_.cia1Icr : s_.cia2Icr;
    icr = static_cast<Byte>(icr | ((index & 1) ? 0x02 : 0x01));
    if (control(index) & 0x08) control(index) = static_cast<Byte>(control(index) & ~1);  // one-shot: stops
    // timer B can count the underflows of timer A
    if (!(index & 1)) {
        const unsigned b = index + 1;
        if ((control(b) & 1) && (control(b) & 0x40)) {
            State::Timer& tb = s_.timers[b];
            std::uint64_t left = n;
            while (left > 0 && (control(b) & 1)) {
                if (left <= tb.counter) {
                    tb.counter = static_cast<std::uint16_t>(tb.counter - left);
                    left = 0;
                } else {
                    left -= std::uint64_t{tb.counter} + 1;
                    tb.counter = tb.latch;
                    timerUnderflows(b, 1);
                }
            }
        }
    }
}

void C64::writeTimer(unsigned cia, unsigned r, Byte v) {
    std::array<Byte, 0x10>& regs = cia == 0 ? s_.cia1 : s_.cia2;
    const unsigned index = cia * 2 + (r >= 6 && r <= 7 ? 1 : r == 0x0F ? 1 : 0);
    State::Timer& t = s_.timers[index];
    if (r >= 4 && r <= 7) {
        regs[r] = v;
        const unsigned lo = (index & 1) ? 6 : 4;
        t.latch = static_cast<std::uint16_t>(regs[lo] | (regs[lo + 1] << 8));
        if ((r & 1) && !(control(index) & 1)) t.counter = t.latch;  // the high byte loads a stopped timer
        return;
    }
    // control register
    const bool wasRunning = (regs[r] & 1) != 0;
    if (wasRunning) t.counter = static_cast<std::uint16_t>(timerValue(index));  // freeze the current value
    regs[r] = static_cast<Byte>(v & ~0x10);
    if (v & 0x10) t.counter = t.latch;  // force load
    const std::uint64_t now = cpu_.cycleCount();
    t.start = now;
    t.next = now + t.counter + 1;
}

void C64::updateNmi() {
    const bool line = (s_.cia2Icr & s_.cia2Mask & 0x1F) != 0;
    if (line && !s_.nmiLine) cpu_.nmi();  // the NMI input reacts to the falling edge of the CIA's output
    s_.nmiLine = line;
}

Byte C64::peek(Address a) const {
    if (cpu_.portEnabled() && a < 2) return a == 0 ? cpu_.portDirection() : cpu_.portValue();
    Byte v = 0;
    if (const_cast<C64*>(this)->readMapped(a, v, false)) return v;  // without side effects
    return ram_.read8(a);
}

bool C64::readIo(Address a, Byte& v, bool effects) {
    if (a < 0xD400) {
        const unsigned r = a & 0x3F;
        const unsigned line = rasterLine();
        switch (r) {
            case 0x11: v = static_cast<Byte>((vic_[0x11] & 0x7F) | ((line & 0x100) >> 1)); break;
            case 0x12: v = static_cast<Byte>(line); break;
            case 0x16: v = static_cast<Byte>(vic_[0x16] | 0xC0); break;
            case 0x18: v = static_cast<Byte>(vic_[0x18] | 0x01); break;
            case 0x19: v = static_cast<Byte>(s_.vicIrq | 0x70 | ((s_.vicIrq & vic_[0x1A] & 0x0F) ? 0x80 : 0)); break;
            case 0x1A: v = static_cast<Byte>(vic_[0x1A] | 0xF0); break;
            case 0x1E:
            case 0x1F: v = 0; break;  // no sprite collisions
            default:
                if (r >= 0x2F) v = 0xFF;
                else if (r >= 0x20) v = static_cast<Byte>(vic_[r] | 0xF0);
                else v = vic_[r];
        }
        return true;
    }
    if (a < 0xD800) {
        const unsigned r = a & 0x1F;
        if (r == 0x19 || r == 0x1A) v = 0xFF;  // no paddles
        else if (r == 0x1B) {                    // oscillator 3: pseudo random
            std::uint32_t x = s_.random;
            if (!effects) {
                v = static_cast<Byte>(x >> 24);
                return true;
            }
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            s_.random = x;
            v = static_cast<Byte>(x >> 24);
        } else v = 0;                            // write-only registers
        return true;
    }
    if (a < 0xDC00) {
        v = static_cast<Byte>(color_[a & 0x3FF] & 0x0F);
        return true;
    }
    if (a < 0xDD00) {
        const unsigned r = a & 0x0F;
        switch (r) {
            case 0x00: v = static_cast<Byte>((s_.cia1[0] | ~s_.cia1[2]) & ~joy2_); break;
            case 0x01: {
                // keyboard rows: a pressed key pulls its row low while its column is driven low through port A
                const Byte columns = static_cast<Byte>(s_.cia1[0] | ~s_.cia1[2]);
                Byte rows = 0xFF;
                for (unsigned c = 0; c < 8; ++c)
                    if (!((columns >> c) & 1)) rows = static_cast<Byte>(rows & ~keys_[c]);
                v = static_cast<Byte>((rows & (s_.cia1[1] | ~s_.cia1[3])) & ~joy1_);
                break;
            }
            case 0x04: v = static_cast<Byte>(timerValue(0)); break;
            case 0x05: v = static_cast<Byte>(timerValue(0) >> 8); break;
            case 0x06: v = static_cast<Byte>(timerValue(1)); break;
            case 0x07: v = static_cast<Byte>(timerValue(1) >> 8); break;
            case 0x0D:
                v = static_cast<Byte>(s_.cia1Icr | ((s_.cia1Icr & s_.cia1Mask) ? 0x80 : 0));
                if (effects) {
                    s_.cia1Icr = 0;  // reading acknowledges
                    updateIrq();
                }
                break;
            default: v = s_.cia1[r];
        }
        return true;
    }
    if (a < 0xDE00) {
        const unsigned r = a & 0x0F;
        if (r == 0x00) v = static_cast<Byte>(s_.cia2[0] | ~s_.cia2[2]);
        else if (r >= 4 && r <= 7) v = static_cast<Byte>(timerValue(r < 6 ? 2 : 3) >> ((r & 1) * 8));
        else if (r == 0x0D) {
            v = static_cast<Byte>(s_.cia2Icr | ((s_.cia2Icr & s_.cia2Mask) ? 0x80 : 0));
            if (effects) {
                s_.cia2Icr = 0;  // reading acknowledges
                updateNmi();
            }
        } else v = s_.cia2[r];
        return true;
    }
    v = 0xFF;  // I/O 1 / I/O 2: nothing connected
    return true;
}

bool C64::writeIo(Address a, Byte v) {
    IoWrite w;
    w.value = v;
    w.pc = cpu_.instructionAddress();
    if (a < 0xD400) {
        const unsigned r = a & 0x3F;
        w.address = static_cast<Address>(0xD000 + r);
        w.chip = "VIC";
        if (r == 0x19) {
            s_.vicIrq = static_cast<Byte>(s_.vicIrq & ~v & 0x0F);  // writing 1 acknowledges
            updateIrq();
        } else {
            vic_[r] = v;
            if (r == 0x1A) updateIrq();
        }
    } else if (a < 0xD800) {
        const unsigned r = a & 0x1F;
        w.address = static_cast<Address>(0xD400 + r);
        w.chip = "SID";
        s_.sid[r] = v;
    } else if (a < 0xDC00) {
        w.address = a;
        w.chip = "COLOR";
        color_[a & 0x3FF] = static_cast<Byte>(v & 0x0F);
    } else if (a < 0xDD00) {
        const unsigned r = a & 0x0F;
        w.address = static_cast<Address>(0xDC00 + r);
        w.chip = "CIA1";
        if (r == 0x0D) {
            if (v & 0x80) s_.cia1Mask = static_cast<Byte>(s_.cia1Mask | (v & 0x1F));
            else s_.cia1Mask = static_cast<Byte>(s_.cia1Mask & ~v);
            updateIrq();
        } else if ((r >= 4 && r <= 7) || r == 0x0E || r == 0x0F) {
            writeTimer(0, r, v);
        } else {
            s_.cia1[r] = v;
        }
    } else if (a < 0xDE00) {
        const unsigned r = a & 0x0F;
        w.address = static_cast<Address>(0xDD00 + r);
        w.chip = "CIA2";
        if (r == 0x0D) {
            if (v & 0x80) s_.cia2Mask = static_cast<Byte>(s_.cia2Mask | (v & 0x1F));
            else s_.cia2Mask = static_cast<Byte>(s_.cia2Mask & ~v);
            updateNmi();
        } else if ((r >= 4 && r <= 7) || r == 0x0E || r == 0x0F) {
            writeTimer(1, r, v);
        } else {
            s_.cia2[r] = v;
        }
    } else {
        w.address = a;
        w.chip = a < 0xDF00 ? "IO1" : "IO2";
    }
    w.name = registerName(w.address);
    log_.push_back(std::move(w));
    if (log_.size() > 2 * maxIoLog) log_.erase(log_.begin(), log_.end() - static_cast<std::ptrdiff_t>(maxIoLog));
    return true;
}

std::vector<IoWrite> C64::takeIoLog() {
    std::vector<IoWrite> out;
    const std::size_t n = std::min(log_.size(), maxIoLog);
    out.assign(log_.end() - static_cast<std::ptrdiff_t>(n), log_.end());
    log_.clear();
    return out;
}

std::string C64::registerName(Address a) {
    if (a >= 0xD000 && a < 0xD400) {
        const unsigned r = a & 0x3F;
        return r < 0x2F ? kVicNames[r] : "";
    }
    if (a >= 0xD400 && a < 0xD800) {
        const unsigned r = a & 0x1F;
        return r < 0x1D ? kSidNames[r] : "";
    }
    if (a >= 0xDC00 && a < 0xDD00) return kCia1Names[a & 0x0F];
    if (a >= 0xDD00 && a < 0xDE00) return kCia2Names[a & 0x0F];
    return "";
}

void C64::tick() {
    const std::uint64_t cycles = cpu_.cycleCount();
    // raster interrupt: did the beam pass the compare line since the last check?
    const std::uint64_t line = cycles / kCyclesPerLine;
    const std::uint64_t prev = s_.lastRasterCheck;
    if (line > prev) {
        const unsigned cmp = static_cast<unsigned>(vic_[0x12] | ((vic_[0x11] & 0x80) << 1));
        const std::uint64_t passed = line - prev;
        const unsigned from = static_cast<unsigned>((prev + 1) % kRasterLines);
        const unsigned dist = (cmp + kRasterLines - from) % kRasterLines;
        if (cmp < kRasterLines && (passed >= kRasterLines || dist < passed)) s_.vicIrq = static_cast<Byte>(s_.vicIrq | 0x01);
        s_.lastRasterCheck = line;
    }
    // CIA timers counting cycles (continuous or one-shot)
    for (unsigned i = 0; i < 4; ++i) {
        State::Timer& t = s_.timers[i];
        if (!(control(i) & 1) || !countsCycles(i) || cycles < t.next) continue;
        const std::uint64_t period = std::uint64_t{t.latch} + 1;
        const std::uint64_t n = control(i) & 0x08 ? 1 : (cycles - t.next) / period + 1;
        const std::uint64_t last = t.next + (n - 1) * period;  // the most recent underflow: the counter was reloaded with the latch
        t.start = last;
        t.counter = t.latch;
        t.next = last + period;
        timerUnderflows(i, n);
    }
    updateNmi();
    updateIrq();
}

void C64::updateIrq() {
    const bool vic = (s_.vicIrq & vic_[0x1A] & 0x0F) != 0;
    const bool cia = (s_.cia1Icr & s_.cia1Mask & 0x1F) != 0;
    cpu_.setIrq(vic || cia || s_.externalIrq);
}

C64::State C64::state() const { return s_; }

void C64::setState(const State& s) {
    s_ = s;
    updateIrq();
}

void C64::resetChips() {
    State fresh;
    fresh.config = static_cast<Byte>(cpu_.portValue() & 7);
    fresh.lastRasterCheck = cpu_.cycleCount() / kCyclesPerLine;
    s_ = fresh;
    log_.clear();
    updateIrq();
}

}  // namespace m6510
