#include "m6510/kernal.hpp"

#include <algorithm>

#include "m6510/format.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

// Jump table ($FF81-$FFF3).
const std::map<Address, std::string>& table() {
    static const std::map<Address, std::string> t = {
        {0xFF81, "CINT"},   {0xFF84, "IOINIT"}, {0xFF87, "RAMTAS"}, {0xFF8A, "RESTOR"}, {0xFF8D, "VECTOR"},
        {0xFF90, "SETMSG"}, {0xFF93, "SECOND"}, {0xFF96, "TKSA"},   {0xFF99, "MEMTOP"}, {0xFF9C, "MEMBOT"},
        {0xFF9F, "SCNKEY"}, {0xFFA2, "SETTMO"}, {0xFFA5, "ACPTR"},  {0xFFA8, "CIOUT"},  {0xFFAB, "UNTLK"},
        {0xFFAE, "UNLSN"},  {0xFFB1, "LISTEN"}, {0xFFB4, "TALK"},   {0xFFB7, "READST"}, {0xFFBA, "SETLFS"},
        {0xFFBD, "SETNAM"}, {0xFFC0, "OPEN"},   {0xFFC3, "CLOSE"},  {0xFFC6, "CHKIN"},  {0xFFC9, "CHKOUT"},
        {0xFFCC, "CLRCHN"}, {0xFFCF, "CHRIN"},  {0xFFD2, "CHROUT"}, {0xFFD5, "LOAD"},   {0xFFD8, "SAVE"},
        {0xFFDB, "SETTIM"}, {0xFFDE, "RDTIM"},  {0xFFE1, "STOP"},   {0xFFE4, "GETIN"},  {0xFFE7, "CLALL"},
        {0xFFEA, "UDTIM"},  {0xFFED, "SCREEN"}, {0xFFF0, "PLOT"},   {0xFFF3, "IOBASE"},
    };
    return t;
}

// Entries that go through a RAM vector: table address -> (vector, routine address in the real KERNAL).
struct Vectored {
    Address entry, vector, routine;
    const char* name;
};
constexpr Vectored kVectored[] = {
    {0xFFC0, 0x031A, 0xF34A, "OPEN"},  {0xFFC3, 0x031C, 0xF291, "CLOSE"},  {0xFFC6, 0x031E, 0xF20E, "CHKIN"},
    {0xFFC9, 0x0320, 0xF250, "CHKOUT"}, {0xFFCC, 0x0322, 0xF333, "CLRCHN"}, {0xFFCF, 0x0324, 0xF157, "CHRIN"},
    {0xFFD2, 0x0326, 0xF1CA, "CHROUT"}, {0xFFE1, 0x0328, 0xF6ED, "STOP"},   {0xFFE4, 0x032A, 0xF13E, "GETIN"},
    {0xFFE7, 0x032C, 0xF32F, "CLALL"},  {0xFFD5, 0x0330, 0xF4A5, "LOAD"},   {0xFFD8, 0x0332, 0xF5ED, "SAVE"},
};

// Default RAM vectors $0314-$0333 (RESTOR).
constexpr Word kVectors[16] = {0xEA31, 0xFE66, 0xFE47, 0xF34A, 0xF291, 0xF20E, 0xF250, 0xF333,
                               0xF157, 0xF1CA, 0xF6ED, 0xF13E, 0xF32F, 0xFE66, 0xF4A5, 0xF5ED};

// Real 6502 code in the generated image: [start, end).
struct Code {
    Address start;
    std::vector<Byte> bytes;
};
const std::vector<Code>& realCode() {
    static const std::vector<Code> c = {
        // IRQ/BRK entry: save registers, BRK -> ($0316), IRQ -> ($0314)
        {0xFF48, {0x48, 0x8A, 0x48, 0x98, 0x48, 0xBA, 0xBD, 0x04, 0x01, 0x29, 0x10, 0xF0, 0x03, 0x6C, 0x16, 0x03, 0x6C, 0x14, 0x03}},
        // default IRQ handler: update the clock, acknowledge CIA 1, restore registers
        {0xEA31, {0x20, 0xEA, 0xFF, 0xAD, 0x0D, 0xDC, 0x4C, 0x81, 0xEA}},
        {0xEA81, {0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40}},
        // NMI entry and default handler (acknowledge CIA 2)
        {0xFE43, {0x78, 0x6C, 0x18, 0x03}},
        {0xFE47, {0x48, 0x8A, 0x48, 0x98, 0x48, 0xAD, 0x0D, 0xDD, 0x4C, 0xBC, 0xFE}},
        {0xFEBC, {0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40}},
        // LOAD: store the address like the real KERNAL, then through the vector
        {0xF49E, {0x86, 0xC3, 0x84, 0xC4, 0x6C, 0x30, 0x03}},
    };
    return c;
}

bool isRealCode(Address pc) {
    for (const Code& c : realCode())
        if (pc >= c.start && pc < c.start + c.bytes.size()) return true;
    for (const Vectored& v : kVectored)
        if (pc >= v.entry && pc < v.entry + 3) return true;
    return false;
}

const char* colorCode(Byte c, int& color) {
    static const std::pair<Byte, int> map[] = {{0x90, 0},  {0x05, 1},  {0x1C, 2},  {0x9F, 3}, {0x9C, 4}, {0x1E, 5},
                                               {0x1F, 6},  {0x9E, 7},  {0x81, 8},  {0x95, 9}, {0x96, 10}, {0x97, 11},
                                               {0x98, 12}, {0x99, 13}, {0x9A, 14}, {0x9B, 15}};
    for (const auto& [code, col] : map) {
        if (code == c) {
            color = col;
            return "";
        }
    }
    return nullptr;
}

// PETSCII -> screen code (without reverse).
Byte screenCode(Byte c) {
    if (c < 0x20) return c;
    if (c < 0x40) return c;
    if (c < 0x60) return static_cast<Byte>(c - 0x40);
    if (c < 0x80) return static_cast<Byte>(c - 0x20);
    if (c < 0xA0) return static_cast<Byte>(c - 0x40);  // control range: not printed
    if (c < 0xC0) return static_cast<Byte>(c - 0x40);
    if (c < 0xFF) return static_cast<Byte>(c - 0x80);
    return 0x5E;
}

}  // namespace

Kernal::Kernal(C64& c64, Cpu& cpu, Memory& ram) : c64_(c64), cpu_(cpu), ram_(ram) {}

Kernal::~Kernal() {
    cpu_.executeHook = nullptr;
    cpu_.hookLow = 1;
    cpu_.hookHigh = 0;
}

const std::map<Address, std::string>& Kernal::jumpTable() { return table(); }

std::string Kernal::entryName(Address a) {
    if (auto it = table().find(a); it != table().end()) return it->second;
    for (const Vectored& v : kVectored)
        if (v.routine == a) return v.name;
    if (a == 0xFE66) return "BRK/warm start";
    if (a == 0xE544) return "CLRSCR";   // clear the screen (called directly by many programs)
    if (a == 0xE566) return "HOME";     // cursor home
    if (a == 0xFCE2) return "RESET";
    if (a == kExitTrap) return "return to BASIC";
    return "";
}

std::string Kernal::describeAddress(Address a) {
    switch (a) {
        case 0x0314: return "CINV: IRQ vector (RAM), normally $EA31";
        case 0x0315: return "CINV+1: high byte of the IRQ vector";
        case 0x0316: return "CBINV: BRK vector (RAM)";
        case 0x0317: return "CBINV+1: high byte of the BRK vector";
        case 0x0318: return "NMINV: NMI vector (RAM)";
        case 0x0319: return "NMINV+1: high byte of the NMI vector";
        case 0xEA31: return "the KERNAL's IRQ handler (clock, cursor, keyboard); it ends at $EA81";
        case 0xEA81: return "the end of the KERNAL's IRQ handler: restores Y, X, A from the stack and returns with RTI";
        case 0xFE47: return "the KERNAL's NMI handler";
        case 0xFF48: return "the KERNAL's IRQ/BRK entry: pushes A, X, Y and jumps through CINV ($0314) or CBINV ($0316)";
        default: break;
    }
    if (a >= 0x07F8 && a <= 0x07FF) return "sprite pointer " + std::to_string(a - 0x07F8) + " (with the screen at $0400): block = value * 64";
    if (a >= 0x0400 && a < 0x07E8) return "screen memory (row " + std::to_string((a - 0x0400) / 40) + ", column " + std::to_string((a - 0x0400) % 40) + ")";
    if (a >= 0xD800 && a < 0xDBE8) return "colour RAM (row " + std::to_string((a - 0xD800) / 40) + ", column " + std::to_string((a - 0xD800) % 40) + ")";
    const std::string name = entryName(a);
    if (!name.empty()) return "KERNAL: " + name;
    if (a >= 0xE000) return "inside the KERNAL ROM";
    if (a >= 0xA000 && a < 0xC000) return "inside the BASIC ROM";
    return "";
}

void Kernal::install() {
    auto& img = c64_.kernalRomForEmulation();
    img.fill(0x60);  // never executed: the hook takes every address that is not real code
    auto put = [&img](Address a, std::initializer_list<Byte> bytes) {
        for (Byte b : bytes) img[static_cast<std::size_t>(a++ - 0xE000)] = b;
    };
    for (const Code& c : realCode())
        for (std::size_t i = 0; i < c.bytes.size(); ++i) img[c.start - 0xE000 + i] = c.bytes[i];
    for (const Vectored& v : kVectored) {
        if (v.entry == 0xFFD5) put(0xFFD5, {0x4C, 0x9E, 0xF4});  // LOAD goes through $F49E
        else put(v.entry, {0x6C, static_cast<Byte>(v.vector), static_cast<Byte>(v.vector >> 8)});
    }
    put(0xFFFA, {0x43, 0xFE, 0xE2, 0xFC, 0x48, 0xFF});  // NMI, RESET, IRQ
    // the table RESTOR copies to $0314-$0333: programs read it directly (LDA $FD30,X / STA $0314,X)
    for (int i = 0; i < 16; ++i) put(static_cast<Address>(0xFD30 + 2 * i), {static_cast<Byte>(kVectors[i]), static_cast<Byte>(kVectors[i] >> 8)});
    c64_.setKernalPresent(true);
    cpu_.hookLow = 0xE000;
    cpu_.hookHigh = 0xFFFF;
    cpu_.executeHook = [this](Cpu& c) { return hook(c); };
}

// ---- machine setup ------------------------------------------------------------------------------------
void Kernal::restor() {
    for (int i = 0; i < 16; ++i) ram_.write16(static_cast<Address>(0x0314 + 2 * i), kVectors[i]);
}

void Kernal::ioinit() {
    c64_.write(0x0000, 0x2F);
    c64_.write(0x0001, 0x37);
    c64_.write(0xDC0D, 0x7F);  // CIA 1: all interrupts off, then timer A at 60 Hz (PAL: $4025)
    c64_.write(0xDC0E, 0x08);
    c64_.write(0xDC04, 0x25);
    c64_.write(0xDC05, 0x40);
    c64_.write(0xDC0D, 0x81);
    c64_.write(0xDC0E, 0x11);
    c64_.write(0xDD0D, 0x7F);
    c64_.write(0xDD02, 0x3F);
    c64_.write(0xDD00, 0x17);  // VIC bank 0
    c64_.write(0xD418, 0x00);
}

void Kernal::ramtas() {
    for (Address a = 0x0002; a < 0x0100; ++a) ram_.write8(a, 0);
    for (Address a = 0x0200; a < 0x0400; ++a) ram_.write8(a, 0);
    ram_.write16(0x0281, 0x0800);  // MEMBOT
    ram_.write16(0x0283, 0xA000);  // MEMTOP
    ram_.write8(0x0288, 0x04);     // screen page
}

void Kernal::cint() {
    c64_.write(0xD011, 0x1B);
    c64_.write(0xD016, 0x08);
    c64_.write(0xD018, 0x14);
    c64_.write(0xD020, 0x0E);
    c64_.write(0xD021, 0x06);
    ram_.write8(0x0286, 0x0E);  // cursor colour: light blue
    ram_.write8(0x0288, 0x04);
    clearScreen();
}

void Kernal::clearScreen() {
    const Address screen = static_cast<Address>(ram_.read8(0x0288) << 8);
    const Byte color = ram_.read8(0x0286);
    for (unsigned i = 0; i < 1000; ++i) {
        ram_.write8(static_cast<Address>(screen + i), 0x20);
        c64_.setColorRam(i, color);
    }
    ram_.write8(0xD3, 0);
    ram_.write8(0xD6, 0);
}

void Kernal::coldStart() {
    ioinit();
    ramtas();
    restor();
    cint();
    ram_.write8(0x009D, 0x80);  // messages on (direct mode)
}

void Kernal::startProgram(Address entry) {
    coldStart();
    (void)c64_.takeIoLog();  // the set-up writes are not the program's
    Registers& r = cpu_.registers();
    r.s = 0xFA;
    cpu_.push(static_cast<Byte>((kExitTrap - 1) >> 8));
    cpu_.push(static_cast<Byte>(kExitTrap - 1));
    r.a = ram_.read8(0x030C);
    r.x = ram_.read8(0x030D);
    r.y = ram_.read8(0x030E);
    r.p = static_cast<Byte>((ram_.read8(0x030F) | kFlagU) & ~(kFlagI | kFlagD | kFlagB));
    r.pc = entry;
    cpu_.setHalted(false);
}

// ---- screen output ----------------------------------------------------------------------------------
void Kernal::scrollUp() {
    const Address screen = static_cast<Address>(ram_.read8(0x0288) << 8);
    for (unsigned i = 0; i < 960; ++i) {
        ram_.write8(static_cast<Address>(screen + i), ram_.read8(static_cast<Address>(screen + i + 40)));
        c64_.setColorRam(i, c64_.colorRam(i + 40));
    }
    for (unsigned i = 960; i < 1000; ++i) {
        ram_.write8(static_cast<Address>(screen + i), 0x20);
        c64_.setColorRam(i, ram_.read8(0x0286));
    }
}

void Kernal::chrout(Byte c) {
    // console text
    if (c == 0x0D || c == 0x8D) console_ += '\n';
    else if (c >= 0x41 && c <= 0x5A) console_ += static_cast<char>(c);
    else if ((c >= 0xC1 && c <= 0xDA) || (c >= 0x61 && c <= 0x7A)) console_ += static_cast<char>('a' + ((c & 0x1F) - 1));
    else console_ += petsciiToText(c);

    // screen
    const Address screen = static_cast<Address>(ram_.read8(0x0288) << 8);
    unsigned col = ram_.read8(0xD3), row = ram_.read8(0xD6);
    if (col >= 40) col = 39;
    if (row >= 25) row = 24;
    int color = -1;
    switch (c) {
        case 0x0D:
        case 0x8D:
            col = 0;
            ++row;
            ram_.write8(0xC7, 0);
            break;
        case 0x93: clearScreen(); return;
        case 0x13: col = row = 0; break;
        case 0x11: ++row; break;
        case 0x91: if (row > 0) --row; break;
        case 0x1D: if (++col == 40) { col = 0; ++row; } break;
        case 0x9D: if (col > 0) --col; else if (row > 0) { col = 39; --row; } break;
        case 0x14:
            if (col > 0) --col;
            ram_.write8(static_cast<Address>(screen + row * 40 + col), 0x20);
            break;
        case 0x12: ram_.write8(0xC7, 1); break;
        case 0x92: ram_.write8(0xC7, 0); break;
        case 0x0E: c64_.write(0xD018, static_cast<Byte>(c64_.vic(0x18) | 0x02)); break;
        case 0x8E: c64_.write(0xD018, static_cast<Byte>(c64_.vic(0x18) & ~0x02)); break;
        default:
            if (colorCode(c, color)) {
                ram_.write8(0x0286, static_cast<Byte>(color));
            } else if ((c >= 0x20 && c < 0x80) || c >= 0xA0) {
                Byte sc = screenCode(c);
                if (ram_.read8(0xC7)) sc = static_cast<Byte>(sc | 0x80);
                ram_.write8(static_cast<Address>(screen + row * 40 + col), sc);
                c64_.setColorRam(row * 40 + col, ram_.read8(0x0286));
                if (++col == 40) {
                    col = 0;
                    ++row;
                }
            }
    }
    if (row >= 25) {
        scrollUp();
        row = 24;
    }
    ram_.write8(0xD3, static_cast<Byte>(col));
    ram_.write8(0xD6, static_cast<Byte>(row));
    ram_.write16(0xD1, static_cast<Word>(screen + row * 40));
}

std::string Kernal::fileName() const {
    const unsigned len = ram_.read8(0xB7);
    const Address p = ram_.read16(0xBB);
    std::string n;
    for (unsigned i = 0; i < len; ++i) {
        const Byte b = ram_.read8(static_cast<Address>(p + i));
        if (b >= 0xC1 && b <= 0xDA) n += static_cast<char>('a' + (b - 0xC1));
        else if (b >= 0x20 && b < 0x7F) n += static_cast<char>(b);
        else n += '?';
    }
    return n;
}

// ---- the hook -------------------------------------------------------------------------------------------
bool Kernal::hook(Cpu& cpu) {
    if (!c64_.kernalVisible()) return false;
    Registers& r = cpu.registers();
    const Address pc = r.pc;
    if (isRealCode(pc)) return false;

    if (pc == kExitTrap) {
        cpu.requestStop("program returned to BASIC");
        return true;
    }
    if (pc == 0xFE66) {  // BRK through $0316 (the IRQ entry has pushed A, X, Y on top of P and PC)
        const Address ret = static_cast<Address>(ram_.read8(static_cast<Address>(0x100 + ((r.s + 5) & 0xFF))) |
                                                 (ram_.read8(static_cast<Address>(0x100 + ((r.s + 6) & 0xFF))) << 8));
        cpu.requestStop("BRK at " + formatHex(static_cast<Address>(ret - 2), 4, NumberStyle::Dollar));
        return true;
    }
    if (pc == 0xFCE2) {  // RESET
        coldStart();
        cpu.requestStop("reset (no BASIC ROM: stopped after the KERNAL initialisation)");
        r.pc = kExitTrap;
        return true;
    }

    KernalCall call;
    call.address = pc;
    call.name = entryName(pc);
    call.before = r;
    const Address ret = static_cast<Address>(ram_.read8(static_cast<Address>(0x100 + ((r.s + 1) & 0xFF))) |
                                             (ram_.read8(static_cast<Address>(0x100 + ((r.s + 2) & 0xFF))) << 8));
    call.callSite = static_cast<Address>(ret - 2);
    if (call.name.empty()) {
        call.note = "unknown KERNAL entry " + formatHex(pc, 4, NumberStyle::Dollar) + ": returns with C = 0";
    } else {
        call.emulated = emulate(call.name, r, call.note);
    }
    if (!call.emulated) r.p = static_cast<Byte>(r.p & ~kFlagC);
    cpu.returnFromSubroutine();
    cpu.addCycles(12);  // JMP/RTS overhead of a short routine (the real time is not modelled)
    call.after = r;
    if (isRealCode(call.callSite)) return true;  // the KERNAL's own IRQ handler calling UDTIM: not logged
    calls_.push_back(std::move(call));
    if (calls_.size() > 2 * maxCalls) calls_.erase(calls_.begin(), calls_.end() - static_cast<std::ptrdiff_t>(maxCalls));
    return true;
}

bool Kernal::emulate(const std::string& n, Registers& r, std::string& note) {
    auto setC = [&r](bool on) { r.p = static_cast<Byte>(on ? (r.p | kFlagC) : (r.p & ~kFlagC)); };
    auto setZ = [&r](bool on) { r.p = static_cast<Byte>(on ? (r.p | kFlagZ) : (r.p & ~kFlagZ)); };
    if (n == "CHROUT") {
        chrout(r.a);
        setC(false);
    } else if (n == "GETIN" || n == "CHRIN") {
        if (keys_.empty()) {
            r.a = n == "CHRIN" ? 0x0D : 0x00;
        } else {
            r.a = keys_.front();
            keys_.pop_front();
        }
        setZ(r.a == 0);
        setC(false);
    } else if (n == "CLRSCR") {
        clearScreen();
        setC(false);
    } else if (n == "HOME") {
        ram_.write8(0xD3, 0);
        ram_.write8(0xD6, 0);
        setC(false);
    } else if (n == "STOP") {
        r.a = 0xFF;
        setZ(false);  // STOP key not pressed
    } else if (n == "SCNKEY" || n == "CLRCHN" || n == "CLALL" || n == "OPEN" || n == "CLOSE" || n == "CHKIN" ||
               n == "CHKOUT") {
        if (n != "SCNKEY" && n != "CLRCHN" && n != "CLALL") {
            const unsigned device = ram_.read8(0xBA);
            if (n == "OPEN" || n == "CHKIN" || n == "CHKOUT") {
                if (device != 0 && device != 3) {
                    note = "device " + std::to_string(device) + ": files are only supported through LOAD/SAVE";
                    return false;
                }
            }
        }
        setC(false);
    } else if (n == "UDTIM") {
        unsigned t = static_cast<unsigned>((ram_.read8(0xA0) << 16) | (ram_.read8(0xA1) << 8) | ram_.read8(0xA2)) + 1;
        if (t >= 0x4F1A01) t = 0;  // 24 hours
        ram_.write8(0xA0, static_cast<Byte>(t >> 16));
        ram_.write8(0xA1, static_cast<Byte>(t >> 8));
        ram_.write8(0xA2, static_cast<Byte>(t));
    } else if (n == "RDTIM") {
        r.a = ram_.read8(0xA2);
        r.x = ram_.read8(0xA1);
        r.y = ram_.read8(0xA0);
    } else if (n == "SETTIM") {
        ram_.write8(0xA2, r.a);
        ram_.write8(0xA1, r.x);
        ram_.write8(0xA0, r.y);
    } else if (n == "PLOT") {
        if (r.flag(kFlagC)) {
            r.x = ram_.read8(0xD6);
            r.y = ram_.read8(0xD3);
        } else {
            ram_.write8(0xD6, static_cast<Byte>(std::min<unsigned>(r.x, 24)));
            ram_.write8(0xD3, static_cast<Byte>(std::min<unsigned>(r.y, 39)));
            ram_.write16(0xD1, static_cast<Word>((ram_.read8(0x0288) << 8) + ram_.read8(0xD6) * 40));
        }
    } else if (n == "SCREEN") {
        r.x = 40;
        r.y = 25;
    } else if (n == "IOBASE") {
        r.x = 0x00;
        r.y = 0xDC;
    } else if (n == "MEMTOP" || n == "MEMBOT") {
        const Address at = n == "MEMTOP" ? 0x0283 : 0x0281;
        if (r.flag(kFlagC)) {
            r.x = ram_.read8(at);
            r.y = ram_.read8(static_cast<Address>(at + 1));
        } else {
            ram_.write8(at, r.x);
            ram_.write8(static_cast<Address>(at + 1), r.y);
        }
    } else if (n == "READST") {
        r.a = ram_.read8(0x90);
    } else if (n == "SETMSG") {
        ram_.write8(0x9D, r.a);
    } else if (n == "SETTMO") {
        ram_.write8(0x0285, r.a);
    } else if (n == "SETLFS") {
        ram_.write8(0xB8, r.a);
        ram_.write8(0xBA, r.x);
        ram_.write8(0xB9, r.y);
    } else if (n == "SETNAM") {
        ram_.write8(0xB7, r.a);
        ram_.write8(0xBB, r.x);
        ram_.write8(0xBC, r.y);
    } else if (n == "LOAD") {
        const std::string name = fileName();
        const unsigned device = ram_.read8(0xBA);
        std::optional<Program> p;
        if (files) p = files(name, device);
        if (!p) {
            note = "file \"" + name + "\" not found on device " + std::to_string(device);
            setC(true);
            r.a = 4;
            return true;
        }
        const bool verify = r.a != 0;
        Address at = p->address;
        if (ram_.read8(0xB9) == 0) at = static_cast<Address>(r.x | (r.y << 8));
        bool same = true;
        for (std::size_t i = 0; i < p->bytes.size() && at + i < 0x10000; ++i) {
            const auto a = static_cast<Address>(at + i);
            if (verify) same = same && ram_.read8(a) == p->bytes[i];
            else ram_.write8(a, p->bytes[i]);
        }
        const auto end = static_cast<Address>(std::min<std::size_t>(at + p->bytes.size(), 0xFFFF));
        r.x = static_cast<Byte>(end);
        r.y = static_cast<Byte>(end >> 8);
        ram_.write16(0xAE, end);
        ram_.write8(0x90, static_cast<Byte>(verify && !same ? 0x10 : 0x00));
        note = (verify ? "verified \"" : "loaded \"") + name + "\" at " + formatHex(at, 4, NumberStyle::Dollar) + "-" +
               formatHex(static_cast<Address>(end - 1), 4, NumberStyle::Dollar);
        setC(false);
    } else if (n == "SAVE") {
        const Address start = ram_.read16(r.a);
        const auto end = static_cast<Address>(r.x | (r.y << 8));
        Program p;
        p.address = start;
        p.name = fileName();
        for (unsigned a = start; a < end; ++a) p.bytes.push_back(ram_.read8(static_cast<Address>(a)));
        note = "saved \"" + p.name + "\" " + formatHex(start, 4, NumberStyle::Dollar) + "-" +
               formatHex(static_cast<Address>(end - 1), 4, NumberStyle::Dollar);
        saved[p.name] = std::move(p);
        setC(false);
    } else if (n == "CINT") {
        cint();
    } else if (n == "IOINIT") {
        ioinit();
    } else if (n == "RAMTAS") {
        ramtas();
    } else if (n == "RESTOR") {
        restor();
    } else if (n == "VECTOR") {
        const auto user = static_cast<Address>(r.x | (r.y << 8));
        for (unsigned i = 0; i < 32; ++i) {
            const auto v = static_cast<Address>(0x0314 + i), u = static_cast<Address>(user + i);
            if (r.flag(kFlagC)) ram_.write8(u, ram_.read8(v));
            else ram_.write8(v, ram_.read8(u));
        }
    } else {
        // serial bus: no devices
        ram_.write8(0x90, static_cast<Byte>(ram_.read8(0x90) | 0x80));
        note = "serial bus: no device present (status $80)";
        return false;
    }
    return true;
}

std::string Kernal::takeConsole() {
    std::string s;
    s.swap(console_);
    return s;
}

std::vector<KernalCall> Kernal::takeCalls() {
    std::vector<KernalCall> out;
    const std::size_t n = std::min(calls_.size(), maxCalls);
    out.assign(calls_.end() - static_cast<std::ptrdiff_t>(n), calls_.end());
    calls_.clear();
    return out;
}

void Kernal::addKeys(const std::string& text) {
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\n') keys_.push_back(0x0D);
        else if (c >= 'a' && c <= 'z') keys_.push_back(static_cast<Byte>(c - 'a' + 0x41));  // unshifted letters
        else if (c >= 'A' && c <= 'Z') keys_.push_back(static_cast<Byte>(c - 'A' + 0xC1));
        else keys_.push_back(static_cast<Byte>(c));
    }
}

}  // namespace m6510
