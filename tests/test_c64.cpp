#include "m6510/c64.hpp"

#include "m6510/assembler.hpp"
#include "m6510/kernal.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

struct Machine {
    Memory ram;
    Cpu cpu{ram};
    C64 c64{ram, cpu};
    Kernal kernal{c64, cpu, ram};
    std::map<std::string, std::int64_t> symbols;

    explicit Machine(bool withKernal = true) {
        if (withKernal) kernal.install();
        cpu.powerOn();
    }
    void load(const std::string& src, Address origin = 0xC000) {
        AssemblerOptions o;
        o.origin = origin;
        const AssemblyResult r = assemble(src, o);
        for (const Diagnostic& d : r.errors) std::cerr << "  test program line " << d.line << ": " << d.message << "\n";
        for (const Segment& s : r.segments) ram.load(s.address, s.bytes);
        symbols = r.symbols;
    }
    std::string screenLine(unsigned row) const {
        std::string s;
        for (unsigned c = 0; c < 40; ++c) {
            const Byte sc = ram.read8(static_cast<Address>(0x0400 + row * 40 + c));
            s += sc >= 1 && sc <= 26 ? static_cast<char>('A' + sc - 1) : (sc >= 0x20 && sc < 0x40 ? static_cast<char>(sc) : '?');
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
};

}  // namespace

int main() {
    // ---- memory configurations ------------------------------------------------------------------
    {
        Machine m(false);
        m.c64.loadRom("basic", std::vector<Byte>(0x2000, 0xBA));
        m.c64.loadRom("kernal", std::vector<Byte>(0x2000, 0xEE));
        m.c64.loadRom("char", std::vector<Byte>(0x1000, 0xCC));
        m.ram.write8(0xA000, 0x11);
        m.ram.write8(0xD000, 0x22);
        m.ram.write8(0xE000, 0x33);
        struct Row {
            Byte port;
            Byte a000, d000, e000;  // 0 = I/O
        };
        const Row rows[] = {
            {0, 0x11, 0x22, 0x33}, {1, 0x11, 0xCC, 0x33}, {2, 0x11, 0xCC, 0xEE}, {3, 0xBA, 0xCC, 0xEE},
            {4, 0x11, 0x22, 0x33}, {5, 0x11, 0x00, 0x33}, {6, 0x11, 0x00, 0xEE}, {7, 0xBA, 0x00, 0xEE},
        };
        for (const Row& row : rows) {
            m.c64.write(0x0000, 0x07);
            m.c64.write(0x0001, row.port);
            CHECK_EQ(m.c64.config(), row.port);
            CHECK_EQ(m.c64.peek(0xA000), row.a000);
            CHECK_EQ(m.c64.peek(0xE000), row.e000);
            if (row.d000) CHECK_EQ(m.c64.peek(0xD000), row.d000);
            else CHECK(m.c64.ioVisible());
            CHECK_EQ(m.cpu.peek(0xA000), row.a000);  // the processor sees the same
        }
        // writes go to the RAM underneath ROM
        m.load(" LDA #$55\n STA $A000\n STA $E000\n LDA $A000\n BRK");
        m.c64.write(0x0001, 0x37);
        m.cpu.registers().pc = 0xC000;
        m.cpu.run(4);
        CHECK_EQ(m.ram.read8(0xA000), Byte{0x55});
        CHECK_EQ(m.ram.read8(0xE000), Byte{0x55});
        CHECK_EQ(m.cpu.registers().a, Byte{0xBA});  // but reads see BASIC
        CHECK_EQ(m.c64.describeConfig(), std::string("$A000:BASIC $D000:I/O $E000:KERNAL"));
        // ROM size checks
        bool threw = false;
        try {
            m.c64.loadRom("basic", std::vector<Byte>(100));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- I/O registers and the write log --------------------------------------------------------
    {
        Machine m(false);
        m.c64.write(0x0001, 0x37);
        m.load(" LDA #1\n STA $D020\n STA $D060\n STA $D418\n STA $D800\n INC $D019\n LDA $D800\n BRK");
        m.cpu.registers().pc = 0xC000;
        m.cpu.run(7);
        const std::vector<IoWrite> log = m.c64.takeIoLog();
        CHECK_EQ(log.size(), std::size_t{6});  // INC writes twice
        if (log.size() == 6) {
            CHECK_EQ(log[0].address, Address{0xD020});
            CHECK_EQ(log[0].name, std::string("EXTCOL"));
            CHECK_EQ(log[0].pc, Address{0xC002});
            CHECK_EQ(log[1].address, Address{0xD020});  // $D060 mirrors $D020
            CHECK_EQ(log[2].name, std::string("SIGVOL"));
            CHECK_EQ(log[3].chip, std::string("COLOR"));
            CHECK_EQ(log[4].name, std::string("VICIRQ"));
        }
        CHECK_EQ(m.c64.vic(0x20), Byte{1});
        CHECK_EQ(m.c64.colorRam(0), Byte{1});
        CHECK_EQ(m.ram.read8(0xD020), Byte{0});  // the RAM underneath is untouched
        CHECK(m.c64.takeIoLog().empty());
        CHECK_EQ(C64::registerName(0xDC0D), std::string("CIAICR"));
        CHECK_EQ(C64::registerName(0xDD00), std::string("CI2PRA"));
        CHECK_EQ(C64::registerName(0xD02E), std::string("SP7COL"));
    }

    // ---- raster counter: a wait loop ends, the raster IRQ fires ----------------------------------
    {
        Machine m(false);
        m.c64.write(0x0001, 0x35);
        m.load("wait: LDA $D012\n CMP #$80\n BNE wait\n BRK");
        m.cpu.registers().pc = 0xC000;
        m.cpu.addBreakpoint(0xC007);
        const RunResult r = m.cpu.run(100000);
        CHECK(r.reason == StopReason::Breakpoint);
        CHECK_EQ(m.c64.rasterLine(), 0x80U);
    }
    {
        Machine m(false);
        m.c64.write(0x0001, 0x35);
        m.load(
            "        SEI\n"
            "        LDA #<irq\n STA $FFFE\n LDA #>irq\n STA $FFFF\n"
            "        LDA #$40\n STA $D012\n LDA #$1B\n STA $D011\n"
            "        LDA #1\n STA $D01A\n"
            "        CLI\n"
            "loop:   JMP loop\n"
            "irq:    INC $C200\n"
            "        LDA #1\n STA $D019\n"
            "        RTI\n");
        m.cpu.registers().pc = 0xC000;
        m.cpu.run(200000);  // about 3 frames
        CHECK(m.cpu.interruptCount() >= 2);
        CHECK_EQ(m.ram.read8(0xC200), static_cast<Byte>(m.cpu.interruptCount()));
    }

    // ---- CIA 1 timer A ----------------------------------------------------------------------------
    {
        Machine m(false);
        m.c64.write(0x0001, 0x35);
        m.c64.write(0xDC04, 0xE8);  // 1000 cycles
        m.c64.write(0xDC05, 0x03);
        m.c64.write(0xDC0D, 0x81);
        m.c64.write(0xDC0E, 0x11);
        m.load(" CLI\nloop: JMP loop\nirq: LDA $DC0D\n INX\n RTI");
        m.ram.write16(0xFFFE, static_cast<Word>(m.symbols["irq"]));
        m.cpu.registers().pc = 0xC000;
        m.cpu.registers().x = 0;
        m.cpu.run(10000 / 3);  // JMP = 3 cycles: about 10 000 cycles
        CHECK(m.cpu.registers().x >= 8 && m.cpu.registers().x <= 11);
        const unsigned t = static_cast<unsigned>(m.c64.peek(0xDC04) | (m.c64.peek(0xDC05) << 8));
        CHECK(t <= 1000);
    }

    // ---- KERNAL emulation --------------------------------------------------------------------------
    {
        Machine m;
        m.load(
            "        LDX #0\n"
            "next:   LDA msg,X\n"
            "        BEQ done\n"
            "        JSR $FFD2\n"
            "        INX\n"
            "        BNE next\n"
            "done:   JSR $FFE4\n"  // GETIN
            "        STA key\n"
            "        SEC\n"
            "        JSR $FFF0\n"  // PLOT: read cursor
            "        STX row\n"
            "        STY col\n"
            "        JSR $FFED\n"  // SCREEN
            "        JSR $FF93\n"  // SECOND: not emulated
            "        RTS\n"
            "msg:    .BYTE $93,\"HELLO\",$0D,\"C64\",0\n"
            "key:    .BYTE 0\n"
            "row:    .BYTE 0\n"
            "col:    .BYTE 0\n",
            0x0810);
        m.kernal.addKeys("x");
        m.kernal.startProgram(0x0810);
        const RunResult r = m.cpu.run(100000);
        CHECK(r.reason == StopReason::Requested);
        CHECK_EQ(m.cpu.stopMessage(), std::string("program returned to BASIC"));
        CHECK_EQ(m.kernal.takeConsole(), std::string("{CLR}HELLO\nC64"));
        CHECK_EQ(m.screenLine(0), std::string("HELLO"));
        CHECK_EQ(m.screenLine(1), std::string("C64"));
        CHECK_EQ(m.ram.read8(static_cast<Address>(m.symbols["key"])), Byte{0x58});  // 'x' -> PETSCII $58
        CHECK_EQ(m.ram.read8(static_cast<Address>(m.symbols["row"])), Byte{1});
        CHECK_EQ(m.ram.read8(static_cast<Address>(m.symbols["col"])), Byte{3});
        CHECK_EQ(m.cpu.registers().x, Byte{40});
        CHECK_EQ(m.c64.colorRam(0), Byte{0x0E});
        const std::vector<KernalCall> calls = m.kernal.takeCalls();
        CHECK_EQ(calls.size(), std::size_t{14});  // 10 CHROUT, GETIN, PLOT, SCREEN, SECOND
        if (calls.size() == 14) {
            CHECK_EQ(calls[0].name, std::string("CHROUT"));
            CHECK_EQ(calls[0].address, Address{0xF1CA});  // through the vector at $0326
            CHECK_EQ(calls[0].callSite, Address{0x0817});
            CHECK(calls[0].emulated);
            CHECK_EQ(calls[13].name, std::string("SECOND"));
            CHECK(!calls[13].emulated);
        }
        CHECK(m.kernal.takeCalls().empty());
    }
    // a redirected CHROUT vector is honoured
    {
        Machine m;
        m.load(" LDA #<mine\n STA $0326\n LDA #>mine\n STA $0327\n LDA #65\n JSR $FFD2\n RTS\nmine: STA $C100\n RTS", 0xC000);
        m.kernal.startProgram(0xC000);
        m.ram.write16(0x0326, 0xF1CA);  // startProgram restored the vectors; the program sets its own
        m.cpu.run(1000);
        CHECK_EQ(m.ram.read8(0xC100), Byte{65});
        CHECK_EQ(m.kernal.takeConsole(), std::string(""));
    }
    // the KERNAL's IRQ handler runs the jiffy clock (CIA 1 timer A at 60 Hz)
    {
        Machine m;
        m.load("loop: JMP loop", 0xC000);
        m.kernal.startProgram(0xC000);
        m.cpu.run(985248 / 3);  // about one second
        const unsigned jiffies = static_cast<unsigned>((m.ram.read8(0xA1) << 8) | m.ram.read8(0xA2));
        CHECK(jiffies >= 58 && jiffies <= 62);
        CHECK(m.kernal.takeCalls().empty());  // UDTIM from the KERNAL's own handler is not logged
    }
    // LOAD through the file provider, BRK, unknown entries
    {
        Machine m;
        m.kernal.files = [](const std::string& name, unsigned device) -> std::optional<Program> {
            if (name != "DATA" || device != 8) return std::nullopt;
            return Program{0x3000, {1, 2, 3}, "DATA"};
        };
        m.load(
            "        LDA #4\n LDX #<name\n LDY #>name\n JSR $FFBD\n"   // SETNAM
            "        LDA #1\n LDX #8\n LDY #1\n JSR $FFBA\n"           // SETLFS
            "        LDA #0\n JSR $FFD5\n"                             // LOAD
            "        STX endlo\n STY endhi\n"
            "        JSR $E544\n"                                      // unknown entry
            "        BRK\n"
            "name:   .BYTE \"DATA\"\n"
            "endlo:  .BYTE 0\nendhi: .BYTE 0\n",
            0xC000);
        m.kernal.startProgram(0xC000);
        const RunResult r = m.cpu.run(1000);
        CHECK(r.reason == StopReason::Requested);
        CHECK(m.cpu.stopMessage().find("BRK at $C") == 0);
        CHECK_EQ(m.ram.read8(0x3002), Byte{3});
        CHECK_EQ(m.ram.read8(static_cast<Address>(m.symbols["endlo"])), Byte{0x03});
        CHECK_EQ(m.ram.read8(static_cast<Address>(m.symbols["endhi"])), Byte{0x30});
        const auto calls = m.kernal.takeCalls();
        bool sawUnknown = false;
        for (const auto& c : calls) sawUnknown = sawUnknown || (c.address == 0xE544 && !c.emulated && c.name.empty());
        CHECK(sawUnknown);
        // file not found: C = 1, A = 4
        m.load(" LDA #1\n LDX #<n\n LDY #>n\n JSR $FFBD\n LDA #0\n JSR $FFD5\n RTS\nn: .BYTE \"X\"", 0xC100);
        m.kernal.startProgram(0xC100);
        m.cpu.run(1000);
        CHECK_EQ(m.cpu.registers().a, Byte{4});
        CHECK(m.cpu.registers().flag(kFlagC));
    }
    // with the KERNAL banked out ($01 = $35) the hook stays silent: RAM code runs at $E000+
    {
        Machine m;
        m.load(" LDA #$35\n STA $01\n JSR $E000\n STA $C200\n LDA #$37\n STA $01\n RTS", 0xC000);
        m.load(" LDA #$42\n RTS", 0xE000);
        m.kernal.startProgram(0xC000);
        m.cpu.run(1000);
        CHECK_EQ(m.ram.read8(0xC200), Byte{0x42});
        CHECK_EQ(m.cpu.stopMessage(), std::string("program returned to BASIC"));
        CHECK(m.kernal.takeCalls().empty());
    }
    // state for checkpoints
    {
        Machine m;
        m.kernal.addKeys("ab");
        const Kernal::State s = m.kernal.state();
        m.kernal.addKeys("c");
        m.kernal.setState(s);
        CHECK_EQ(m.kernal.state().keys.size(), std::size_t{2});
        m.c64.write(0xD020, 5);
        const C64::State cs = m.c64.state();
        m.c64.write(0xD020, 7);
        m.c64.setState(cs);
        CHECK_EQ(m.c64.vic(0x20), Byte{5});
    }

    return finish("c64");
}
