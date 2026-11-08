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
            "        JSR $E544\n"                                      // clear screen: emulated
            "        JSR $E5A0\n"                                      // unknown entry
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
        bool sawClear = false;
        for (const auto& c : calls) {
            sawUnknown = sawUnknown || (c.address == 0xE5A0 && !c.emulated && c.name.empty());
            sawClear = sawClear || (c.address == 0xE544 && c.emulated && c.name == "CLRSCR");
        }
        CHECK(sawUnknown);
        CHECK(sawClear);
        // the vector table of RESTOR can be read from the KERNAL image
        CHECK_EQ(m.c64.peek(0xFD30), Byte{0x31});
        CHECK_EQ(m.c64.peek(0xFD31), Byte{0xEA});
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

    // ---- CIA timers: both timers of both CIAs, one-shot, continuous, B counting A, IRQ and NMI ------------------
    {
        Machine m(false);
        m.cpu.setPortEnabled(true);
        m.c64.write(0x0001, 0x35);
        m.c64.resetChips();
        // CIA 2 timer B, one-shot, 1000 cycles: a busy loop waits until it has stopped (as a speech driver does)
        m.load(" .CPU 6510X\n LDA #<1000\n STA $DD06\n LDA #>1000\n STA $DD07\n LDA #$19\n STA $DD0F\n"
               "wait: LDA $DD0F\n AND #1\n BNE wait\n JAM", 0xC000);
        m.cpu.registers().pc = 0xC000;
        RunResult r = m.cpu.run(100000);
        CHECK(r.reason == StopReason::Halted);
        CHECK(r.cycles > 1000 && r.cycles < 1100);
        CHECK_EQ(m.c64.peek(0xDD0D) & 0x02, 0x02);  // underflow flag of timer B
        // timer values count down while running and stand still when stopped
        m.c64.write(0xDC06, 0x00);
        m.c64.write(0xDC07, 0x10);  // latch $1000; the high byte loads the stopped timer
        CHECK_EQ(m.c64.peek(0xDC07), Byte{0x10});
        m.c64.write(0xDC0F, 0x01);  // start, continuous
        m.cpu.addCycles(0);
        CpuState st = m.cpu.state();
        st.cycles += 0x100;
        m.cpu.setState(st);
        CHECK_EQ(m.c64.peek(0xDC07), Byte{0x0F});
        m.c64.write(0xDC0F, 0x00);  // stop
        const Byte frozen = m.c64.peek(0xDC06);
        st.cycles += 0x200;
        m.cpu.setState(st);
        CHECK_EQ(m.c64.peek(0xDC06), frozen);
        // timer B counting the underflows of timer A: A = 9 (period 10 cycles), B = 4 -> B underflows after 50 cycles
        Machine n(false);
        n.c64.resetChips();
        n.c64.write(0xDC04, 9);
        n.c64.write(0xDC05, 0);
        n.c64.write(0xDC06, 4);
        n.c64.write(0xDC07, 0);
        n.c64.write(0xDC0F, 0x41);  // B: count A underflows, start
        n.c64.write(0xDC0E, 0x11);  // A: force load, start
        n.load("loop: NOP\n JMP loop", 0xC000);
        n.cpu.registers().pc = 0xC000;
        n.cpu.run(10);  // 10 x 3 or 2 cycles = 25 cycles: no B underflow yet
        CHECK_EQ(n.c64.peek(0xDC0D) & 0x02, 0);
        n.cpu.run(20);
        CHECK_EQ(n.c64.peek(0xDC0D) & 0x03, 0x03);
        // CIA 2 timer A with NMI: the handler counts, the NMI is an edge (one per underflow, after the ICR is read)
        Machine q(false);
        q.cpu.setPortEnabled(true);
        q.c64.write(0x0001, 0x35);  // KERNAL out: the hardware vectors are RAM
        q.c64.resetChips();
        q.load("main: LDA #<99\n STA $DD04\n LDA #0\n STA $DD05\n LDA #$81\n STA $DD0D\n LDA #$11\n STA $DD0E\n"
               "loop: JMP loop\n"
               "nmi: INC $10\n BIT $DD0D\n RTI", 0xC000);
        q.ram.write16(0xFFFA, static_cast<Address>(q.symbols["nmi"]));
        q.cpu.registers().pc = 0xC000;
        r = q.cpu.run(20000);
        // one NMI per 100 cycles (latch 99): the counter byte wraps, the interrupt count does not
        const std::uint64_t expected = r.cycles / 100;
        CHECK(q.cpu.interruptCount() + 2 >= expected && q.cpu.interruptCount() <= expected + 1);
        CHECK_EQ(q.ram.read8(0x10), static_cast<Byte>(q.cpu.interruptCount()));
        // without acknowledging the ICR the line stays low: only one NMI
        Machine z(false);
        z.cpu.setPortEnabled(true);
        z.c64.write(0x0001, 0x35);
        z.c64.resetChips();
        z.load("main: LDA #<99\n STA $DD04\n LDA #0\n STA $DD05\n LDA #$81\n STA $DD0D\n LDA #$11\n STA $DD0E\n"
               "loop: JMP loop\nnmi: INC $10\n RTI", 0xC000);
        z.ram.write16(0xFFFA, static_cast<Address>(z.symbols["nmi"]));
        z.cpu.registers().pc = 0xC000;
        z.cpu.run(20000);
        CHECK_EQ(z.ram.read8(0x10), Byte{1});
    }

    // ---- input: joystick bits and the keyboard matrix as the CIA ports see them ------------------------------
    {
        Machine m(false);
        m.c64.resetChips();
        CHECK_EQ(m.c64.peek(0xDC00), Byte{0xFF});
        m.c64.setJoystick(2, 0x10 | 0x08);  // fire + right
        CHECK_EQ(m.c64.peek(0xDC00), Byte{0xFF & ~0x18});
        m.c64.setJoystick(1, 0x01);
        CHECK_EQ(m.c64.peek(0xDC01) & 0x1F, 0x1E);
        m.c64.setJoystick(1, 0);
        // SPACE is column 7, row 4: visible when column 7 is selected (PA7 low), not otherwise
        unsigned col = 0, row = 0;
        CHECK(C64::keyPosition("space", col, row));
        CHECK_EQ(col, 7U);
        CHECK_EQ(row, 4U);
        CHECK(!C64::keyPosition("nokey", col, row));
        m.c64.setKey(7, 4, true);
        m.c64.write(0xDC02, 0xFF);  // port A: all outputs
        m.c64.write(0xDC00, 0x7F);  // column 7 selected
        CHECK_EQ(m.c64.peek(0xDC01), Byte{0xEF});
        m.c64.write(0xDC00, 0xFE);  // column 0 selected
        CHECK_EQ(m.c64.peek(0xDC01), Byte{0xFF});
        m.c64.write(0xDC00, 0x00);  // all columns
        CHECK_EQ(m.c64.peek(0xDC01), Byte{0xEF});
        CHECK_EQ(m.c64.pressedKeys().size(), std::size_t{1});
        m.c64.releaseAllKeys();
        CHECK_EQ(m.c64.peek(0xDC01), Byte{0xFF});
    }

    // ---- execution tracking --------------------------------------------------------------------------------------
    {
        Machine m(false);
        m.load(" .CPU 6510X\nstart: LDX #3\nloop: DEX\n BNE loop\n JAM", 0xC000);
        m.cpu.registers().pc = 0xC000;
        m.cpu.trackExecution(true);
        m.cpu.run(100);
        CHECK(m.cpu.executed(0xC000));
        CHECK(m.cpu.executed(0xC002));
        CHECK(!m.cpu.executed(0xC001));
        CHECK_EQ(m.cpu.executedAddresses().size(), std::size_t{4});
        m.cpu.trackExecution(false);
        CHECK(!m.cpu.executed(0xC000));
    }

    return finish("c64");
}
