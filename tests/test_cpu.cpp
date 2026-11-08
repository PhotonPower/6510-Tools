#include "m6510/cpu.hpp"

#include "m6510/assembler.hpp"
#include "m6510/decoder.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

// Assembles `src` at $1000 into `mem`; returns the symbols.
std::map<std::string, std::int64_t> load(Memory& mem, const std::string& src, Address origin = 0x1000) {
    AssemblerOptions o;
    o.origin = origin;
    o.level = CpuLevel::All;
    const AssemblyResult r = assemble(src, o);
    for (const Diagnostic& d : r.errors) std::cerr << "  test program line " << d.line << ": " << d.message << "\n";
    for (const Segment& s : r.segments) mem.load(s.address, s.bytes);
    return r.symbols;
}

struct Machine {
    Memory mem;
    Cpu cpu{mem};
    explicit Machine(const std::string& src = "") {
        mem.write16(kResetVector, 0x1000);
        if (!src.empty()) symbols = load(mem, src);
        cpu.powerOn();
    }
    std::map<std::string, std::int64_t> symbols;
    Registers& r() { return cpu.registers(); }
};

}  // namespace

int main() {
    // ---- reset ---------------------------------------------------------------------------------
    {
        Machine m;
        CHECK_EQ(m.r().pc, Address{0x1000});
        CHECK_EQ(m.r().s, Byte{0xFD});
        CHECK_EQ(m.r().p, Byte{0x24});
        CHECK_EQ(m.r().a, Byte{0});
        CHECK_EQ(m.cpu.cycleCount(), std::uint64_t{7});
    }

    // ---- loads, flags, arithmetic -----------------------------------------------------------------
    {
        Machine m(" LDA #$80\n LDX #0\n LDY #$7F\n CLC\n ADC #$80\n BRK");
        m.cpu.step();
        CHECK_EQ(m.r().a, Byte{0x80});
        CHECK(m.r().flag(kFlagN));
        CHECK(!m.r().flag(kFlagZ));
        m.cpu.step();
        CHECK(m.r().flag(kFlagZ));
        m.cpu.step();
        CHECK(!m.r().flag(kFlagN));
        m.cpu.step();
        m.cpu.step();  // $80 + $80 = $00, C, V
        CHECK_EQ(m.r().a, Byte{0x00});
        CHECK(m.r().flag(kFlagC));
        CHECK(m.r().flag(kFlagV));
        CHECK(m.r().flag(kFlagZ));
    }
    {
        Machine m(" SEC\n LDA #$50\n SBC #$F0\n BRK");  // $50 - $F0 = $60, borrow (C=0), V=0
        m.cpu.run(3);
        CHECK_EQ(m.r().a, Byte{0x60});
        CHECK(!m.r().flag(kFlagC));
        CHECK(!m.r().flag(kFlagV));
    }
    {
        Machine m(" SED\n CLC\n LDA #$58\n ADC #$46\n BRK");  // BCD 58 + 46 = 104
        m.cpu.run(4);
        CHECK_EQ(m.r().a, Byte{0x04});
        CHECK(m.r().flag(kFlagC));
    }
    {
        Machine m(" SED\n SEC\n LDA #$12\n SBC #$21\n BRK");  // BCD 12 - 21 = 91, borrow
        m.cpu.run(4);
        CHECK_EQ(m.r().a, Byte{0x91});
        CHECK(!m.r().flag(kFlagC));
    }
    {
        Machine m(" LDA #$41\n CMP #$41\n BRK");
        m.cpu.run(2);
        CHECK(m.r().flag(kFlagZ));
        CHECK(m.r().flag(kFlagC));
    }
    {
        Machine m(" LDA #$C0\n STA $10\n LDA #$01\n BIT $10\n BRK");
        m.cpu.run(4);
        CHECK(m.r().flag(kFlagN));
        CHECK(m.r().flag(kFlagV));
        CHECK(m.r().flag(kFlagZ));
    }

    // ---- addressing modes and page wraps ----------------------------------------------------------
    {
        Machine m(" LDX #$FF\n LDA $10,X\n LDY #$10\n LDA ($FF),Y\n JMP ($20FF)");
        m.mem.write8(0x000F, 0x11);  // $10 + $FF wraps to $0F in the zero page
        m.mem.write8(0x00FF, 0x00);
        m.mem.write8(0x0000, 0x30);  // pointer at $FF: high byte from $00 -> $3000
        m.mem.write8(0x3010, 0x22);
        m.mem.write8(0x20FF, 0x00);
        m.mem.write8(0x2000, 0x40);  // JMP ($20FF) takes the high byte from $2000
        m.mem.write8(0x2100, 0x50);
        m.cpu.setPortEnabled(false);  // $00 is the pointer's high byte here, not the I/O port
        m.cpu.run(2);
        CHECK_EQ(m.r().a, Byte{0x11});
        m.cpu.run(2);
        CHECK_EQ(m.r().a, Byte{0x22});
        m.cpu.step();
        CHECK_EQ(m.r().pc, Address{0x4000});
    }

    // ---- cycles ---------------------------------------------------------------------------------
    {
        Machine m(" LDX #$01\n LDA $10FF,X\n LDA $1000,X\n STA $10FF,X\nl: BNE l2\n NOP\nl2: BEQ l\n BRK");
        CHECK_EQ(m.cpu.step().cycles, 2U);
        CHECK_EQ(m.cpu.step().cycles, 5U);  // page crossed
        CHECK_EQ(m.cpu.step().cycles, 4U);
        CHECK_EQ(m.cpu.step().cycles, 5U);  // stores always take the extra cycle
        CHECK_EQ(m.cpu.step().cycles, 3U);  // branch taken, same page
        CHECK_EQ(m.cpu.step().cycles, 2U);  // not taken
    }
    {
        Machine m;
        m.mem.load(0x10FD, std::vector<Byte>{0xD0, 0x10});  // BNE from $10FD to $110F: crosses a page
        m.r().pc = 0x10FD;
        m.r().p = 0x24;
        CHECK_EQ(m.cpu.step().cycles, 4U);
        CHECK_EQ(m.r().pc, Address{0x110F});
    }

    // ---- stack, subroutines -----------------------------------------------------------------------
    {
        Machine m(" JSR sub\n BRK\nsub: LDA #7\n RTS");
        m.cpu.step();
        CHECK_EQ(m.r().s, Byte{0xFB});
        CHECK_EQ(m.mem.read8(0x01FD), Byte{0x10});  // return address - 1 = $1002
        CHECK_EQ(m.mem.read8(0x01FC), Byte{0x02});
        CHECK_EQ(m.cpu.callDepth(), 1);
        m.cpu.run(2);
        CHECK_EQ(m.r().pc, Address{0x1003});
        CHECK_EQ(m.r().a, Byte{7});
        CHECK_EQ(m.cpu.callDepth(), 0);
    }
    {
        Machine m(" LDA #$AA\n PHA\n LDA #0\n PLA\n PHP\n PLP\n BRK");
        m.cpu.run(4);
        CHECK_EQ(m.r().a, Byte{0xAA});
        CHECK(m.r().flag(kFlagN));
        m.cpu.step();  // PHP pushes B and bit 5
        CHECK_EQ(m.mem.read8(0x01FD) & 0x30, 0x30);
    }

    // ---- BRK, IRQ, NMI, RTI ------------------------------------------------------------------------
    {
        Machine m(" BRK\n .BYTE $EA\n LDA #1\nhandler: RTI");
        m.mem.write16(kIrqVector, static_cast<Word>(m.symbols["handler"]));
        m.cpu.step();
        CHECK_EQ(m.r().pc, static_cast<Address>(m.symbols["handler"]));
        CHECK(m.r().flag(kFlagI));
        CHECK_EQ(m.mem.read8(0x01FB) & kFlagB, kFlagB);  // pushed P has B set
        CHECK_EQ(m.cpu.interruptCount(), std::uint64_t{1});
        m.cpu.step();  // RTI returns behind the padding byte
        CHECK_EQ(m.r().pc, Address{0x1002});
    }
    {
        Machine m(" CLI\n NOP\n NOP\n NOP\nirq: RTI");
        m.mem.write16(kIrqVector, static_cast<Word>(m.symbols["irq"]));
        m.cpu.setIrq(true);
        m.cpu.step();                    // CLI
        const StepResult s1 = m.cpu.step();  // the NOP after CLI still runs (one-instruction delay)
        CHECK(!s1.interrupt);
        const StepResult s2 = m.cpu.step();  // now the IRQ is taken
        CHECK(s2.interrupt);
        CHECK_EQ(s2.cycles, 7U);
        CHECK_EQ(m.r().pc, static_cast<Address>(m.symbols["irq"]));
        CHECK_EQ(m.mem.read8(0x01FB) & kFlagB, 0);  // B clear for IRQ
        m.cpu.setIrq(false);
        m.cpu.step();  // RTI
        CHECK_EQ(m.r().pc, Address{0x1002});
    }
    {
        Machine m(" SEI\n NOP\nnmi: RTI");
        m.mem.write16(kNmiVector, static_cast<Word>(m.symbols["nmi"]));
        m.cpu.step();
        m.cpu.setIrq(true);  // masked
        CHECK(!m.cpu.step().interrupt);
        m.cpu.nmi();         // not maskable
        CHECK(m.cpu.step().interrupt);
        CHECK_EQ(m.r().pc, static_cast<Address>(m.symbols["nmi"]));
    }

    // ---- JAM ------------------------------------------------------------------------------------
    {
        Machine m(" NOP\n JAM\n NOP");
        const RunResult r = m.cpu.run(100);
        CHECK(r.reason == StopReason::Halted);
        CHECK(m.cpu.halted());
        CHECK_EQ(m.r().pc, Address{0x1001});
        CHECK(m.cpu.step().reason == StopReason::Halted);
    }

    // ---- run control ----------------------------------------------------------------------------
    {
        Machine m("loop: INX\n JMP loop");
        CHECK(m.cpu.run(10).reason == StopReason::Limit);
        CHECK_EQ(m.cpu.instructionCount(), std::uint64_t{10});
        m.cpu.addBreakpoint(0x1001);
        const RunResult r = m.cpu.run(100);
        CHECK(r.reason == StopReason::Breakpoint);
        CHECK_EQ(m.r().pc, Address{0x1001});
        CHECK(m.cpu.run(100).reason == StopReason::Breakpoint);  // continues past the breakpoint it stands on
        CHECK_EQ(r.instructions, 1U);
        m.cpu.clearBreakpoints();
        CHECK(m.cpu.runUntil(0x1000, 10).reason == StopReason::Reached);
    }
    {
        Machine m(" JSR sub\n NOP\n BRK\nsub: LDX #3\nl: DEX\n BNE l\n RTS");
        const RunResult r = m.cpu.stepOver(1000);
        CHECK_EQ(m.r().pc, Address{0x1003});
        CHECK_EQ(r.instructions, 9U);  // JSR, LDX, 3x(DEX,BNE), RTS
        CHECK_EQ(m.r().x, Byte{0});
        m.cpu.registers().pc = static_cast<Address>(m.symbols["sub"]);
        m.cpu.push(0x10);
        m.cpu.push(0x03);  // pretend a JSR from $1001 (returns to $1004)
        CHECK(m.cpu.runUntilReturn(1000).reason == StopReason::Returned);
        CHECK_EQ(m.r().pc, Address{0x1004});
    }
    {
        Machine m("double: ASL A\n RTS\nbad: PLA\n RTS");
        m.r().a = 21;
        const RunResult r = m.cpu.call(0x1000, 0xFFF0, 100);
        CHECK(r.reason == StopReason::Returned);
        CHECK_EQ(m.r().a, Byte{42});
        CHECK_EQ(m.r().s, Byte{0xFD});
        // a routine that pulls one byte too many does not "return" (the stack is off)
        m.mem.write8(0x01FE, 0xEF);
        const RunResult r2 = m.cpu.call(static_cast<Address>(m.symbols["bad"]), 0xFFF0, 100);
        CHECK(r2.reason != StopReason::Returned);
    }
    {
        Machine m(" LDA #1\n STA $2000\n LDA $2001\n BRK");
        m.cpu.addWatchpoint(0x2000, 1, false, true);
        const RunResult r = m.cpu.run(100);
        CHECK(r.reason == StopReason::Watchpoint);
        CHECK_EQ(r.watchAddress, Address{0x2000});
        CHECK_EQ(m.r().pc, Address{0x1005});  // stops after the writing instruction
        m.cpu.clearWatchpoints();
        m.cpu.addWatchpoint(0x2001, 1, true, false);
        CHECK(m.cpu.run(100).reason == StopReason::Watchpoint);
    }

    // ---- hooks ------------------------------------------------------------------------------------
    {
        Machine m(" JSR $FFD2\n JSR $FFD2\n BRK");
        std::string out;
        m.cpu.hookLow = 0xFF00;
        m.cpu.hookHigh = 0xFFFF;
        m.cpu.executeHook = [&out](Cpu& c) {
            out += static_cast<char>('A' + c.registers().a);
            c.registers().a++;
            c.returnFromSubroutine();
            return true;
        };
        m.cpu.run(4);
        CHECK_EQ(out, std::string("AB"));
        CHECK_EQ(m.r().pc, Address{0x1006});
        CHECK_EQ(m.cpu.callDepth(), 0);
        // requestStop
        Machine m2(" JSR $FFD2\n BRK");
        m2.cpu.hookLow = 0xFFD2;
        m2.cpu.hookHigh = 0xFFD2;
        m2.cpu.executeHook = [](Cpu& c) {
            c.requestStop("bye");
            c.returnFromSubroutine();
            return true;
        };
        CHECK(m2.cpu.run(10).reason == StopReason::Requested);
        CHECK_EQ(m2.cpu.stopMessage(), std::string("bye"));
    }
    {
        Machine m(" LDA $D020\n STA $D021\n INC $D019\n BRK");
        std::vector<std::string> log;
        m.cpu.readOverride = [](Address a, Byte& v) {
            if (a != 0xD020) return false;
            v = 0x0E;
            return true;
        };
        m.cpu.writeOverride = [&log](Address a, Byte v) {
            if (a < 0xD000 || a > 0xDFFF) return false;
            log.push_back(hexDigits(a, 4) + "=" + hexDigits(v, 2));
            return true;
        };
        m.cpu.run(3);
        CHECK_EQ(m.r().a, Byte{0x0E});
        CHECK_EQ(m.mem.read8(0xD021), Byte{0});  // swallowed
        CHECK_EQ(log.size(), std::size_t{3});  // STA, and INC writes twice (dummy write)
        if (log.size() == 3) {
            CHECK_EQ(log[1], std::string("D019=00"));
            CHECK_EQ(log[2], std::string("D019=01"));
        }
    }

    // ---- processor port ---------------------------------------------------------------------------
    {
        Machine m(" LDA #$2F\n STA $00\n LDA #$35\n STA $01\n LDA $01\n BRK");
        Byte seenDir = 0, seenData = 0;
        m.cpu.portHook = [&](Byte d, Byte v) {
            seenDir = d;
            seenData = v;
        };
        CHECK_EQ(m.cpu.portValue(), Byte{0x17});  // all inputs after reset
        m.cpu.run(5);
        CHECK_EQ(seenDir, Byte{0x2F});
        CHECK_EQ(seenData, Byte{0x35});
        CHECK_EQ(m.r().a, Byte{0x35});            // (35 & 2F) | (17 & D0) = 25 | 10
        CHECK_EQ(m.mem.read8(0x0001), Byte{0x35});  // the RAM underneath is written too
        m.cpu.setPortEnabled(false);
        CHECK_EQ(m.cpu.peek(0x0001), Byte{0x35});
    }

    // ---- state -------------------------------------------------------------------------------------
    {
        Machine m("loop: INX\n INY\n JMP loop");
        m.cpu.run(7);
        const CpuState s = m.cpu.state();
        const Memory saved = m.mem;
        m.cpu.run(50);
        const Registers after = m.r();
        const std::uint64_t afterCycles = m.cpu.cycleCount();
        m.cpu.setState(s);
        m.mem = saved;
        m.cpu.run(50);
        CHECK_EQ(m.r().x, after.x);
        CHECK_EQ(m.r().y, after.y);
        CHECK_EQ(m.r().pc, after.pc);
        CHECK_EQ(m.cpu.cycleCount(), afterCycles);
    }

    // ---- agreement with the decoder: length and flow of every opcode ------------------------------
    for (int op = 0; op < 256; ++op) {
        if (opInfo(static_cast<Byte>(op)).kind == OpKind::Jam) continue;
        Memory mem;
        mem.load(0x2000, std::vector<Byte>{static_cast<Byte>(op), 0x10, 0x30});
        mem.write16(kIrqVector, 0x5000);
        Cpu cpu(mem);
        cpu.setPortEnabled(false);
        cpu.registers().pc = 0x2000;
        cpu.registers().p = 0x24 | kFlagZ;  // Z set: BNE not taken, BEQ taken
        DecodeOptions o;
        o.level = CpuLevel::All;
        const Instruction in = decode(mem, 0x2000, o);
        cpu.step();
        const Address pc = cpu.registers().pc;
        if (in.flow == FlowKind::Normal) {
            CHECK_EQ(pc, static_cast<Address>(0x2000 + in.length()));
        } else if (in.flow == FlowKind::Jump || in.flow == FlowKind::Call) {
            if (in.target) CHECK_EQ(pc, *in.target);
        } else if (in.flow == FlowKind::CondJump) {
            CHECK(pc == *in.target || pc == static_cast<Address>(0x2002));
        }
    }

    // ---- write tracking for self-extracting programs ------------------------------------------------------------
    {
        Memory mem;
        Cpu cpu(mem);
        cpu.setPortEnabled(false);
        // copy 300 bytes from $2000 to $4000, then jump there; the copied code is LDA #1 / JAM
        const Byte prog[] = {0xA2, 0x00, 0xBD, 0x00, 0x20, 0x9D, 0x00, 0x40, 0xBD, 0x00, 0x21, 0x9D, 0x00, 0x41, 0xE8, 0xD0, 0xF1, 0x4C, 0x00, 0x40};
        mem.load(0x1000, std::vector<Byte>(std::begin(prog), std::end(prog)));
        mem.write8(0x2000, 0xA9);
        mem.write8(0x2001, 0x01);
        mem.write8(0x2002, 0x02);
        cpu.registers().pc = 0x1000;
        CHECK(!cpu.tracking());
        CHECK(!cpu.written(0x4000));
        cpu.trackWrites(true);
        cpu.watchWrittenCode(true, 1024);  // the copy (512 bytes) is too small: runs into the JAM
        RunResult r = cpu.run(100000);
        CHECK(r.reason == StopReason::Halted);
        CHECK_EQ(cpu.writtenBytes(), std::size_t{512});
        CHECK_EQ(cpu.writtenRanges().size(), std::size_t{1});
        CHECK_EQ(cpu.writtenRanges()[0].first, Address{0x4000});
        CHECK_EQ(cpu.writtenRanges()[0].second, std::uint32_t{0x4200});
        CHECK(cpu.writtenRanges(513).empty());
        cpu.setHalted(false);
        cpu.registers().pc = 0x1000;
        cpu.trackWrites(true);
        cpu.watchWrittenCode(true, 256);
        r = cpu.run(100000);
        CHECK(r.reason == StopReason::WrittenCode);
        CHECK_EQ(cpu.registers().pc, Address{0x4000});
        cpu.watchWrittenCode(true, 256, {{0x4000, 0x4100}});  // ignored: it runs on
        CHECK(cpu.run(10).reason == StopReason::Halted);
        cpu.trackWrites(false);
        CHECK_EQ(cpu.writtenBytes(), std::size_t{0});
    }

    return finish("cpu");
}
