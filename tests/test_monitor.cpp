#include "m6510/monitor.hpp"

#include <cstdio>
#include <filesystem>
#include <random>

#include "m6510/assembler.hpp"
#include "m6510/image_io.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool isError(const std::string& text) { return text.rfind("? ", 0) == 0; }

// Types several lines and returns everything the monitor printed.
std::string type(Monitor& m, const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) out += m.execute(l);
    return out;
}

std::string ram(Monitor& m, Address a, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += hexDigits(m.machine().memory().read8(static_cast<Address>(a + i)), 2);
    return s;
}

std::string tempFile(const std::string& name) { return (std::filesystem::temp_directory_path() / name).string(); }

}  // namespace

int main() {
    // ---- numbers: hexadecimal by default ---------------------------------------------------------------
    {
        Monitor m;
        CHECK_EQ(m.execute("e 1000 12 AB \"Hi\" 0"), std::string());
        CHECK_EQ(ram(m, 0x1000, 5), std::string("12AB486900"));
        m.execute("e 1000 100.");  // decimal
        CHECK_EQ(ram(m, 0x1000, 1), std::string("64"));
        m.execute("e 1000 $FF");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("FF"));
        m.execute("e 1000 %1010");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("0A"));
        m.execute("e 1000 0x20");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("20"));
        m.execute("e 1000 1+2*3");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("07"));
        m.execute("e 1000 1B");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("1B"));
        m.execute("e 1000 <D020");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("20"));
        m.execute("sym foo 1004");
        m.execute("e foo 7");
        m.execute("e foo+1 8");
        CHECK_EQ(ram(m, 0x1004, 2), std::string("0708"));
        m.execute("sym dead 5");  // a known symbol wins over the hex number
        m.execute("e 1000 dead");
        CHECK_EQ(ram(m, 0x1000, 1), std::string("05"));
        m.execute("sym -dead");
        m.execute("e.w 1000 dead");
        CHECK_EQ(ram(m, 0x1000, 2), std::string("ADDE"));  // little-endian
        CHECK(isError(m.execute("sym -dead")));
        CHECK(isError(m.execute("e 1000 100")));  // $100 does not fit a byte
        CHECK(isError(m.execute("e 1000 nosuch")));
        CHECK(isError(m.execute("e 1000 1+")));
        CHECK(isError(m.execute("e 1000")));
        CHECK(isError(m.execute("sym 1abc 5")));
        m.execute("r a=12 x=34 y=56");
        m.execute("e 2000 @a @x @y @x+1");
        CHECK_EQ(ram(m, 0x2000, 4), std::string("12345635"));
        CHECK(isError(m.execute("e 2000 @q")));
        CHECK(has(m.execute("sym"), "foo"));
        CHECK(isError(m.execute("frobnicate")));
        CHECK_EQ(m.execute(""), std::string());
        CHECK_EQ(m.execute("   "), std::string());
        CHECK(has(m.execute("?"), "assemble"));
        CHECK(has(Monitor::banner(), "hexadecimal"));
    }

    // ---- memory: dump, fill, compare, copy, search -----------------------------------------------------
    {
        Monitor m;
        m.execute("e 2000 48 45 4C 4C 4F");
        std::string out = m.execute("m 2000 5");
        CHECK_EQ(out, std::string("2000  48 45 4C 4C 4F") + std::string(48 - 14, ' ') + "  HELLO\n");
        out = m.execute("m 2000 14");  // 20 bytes: two lines, the second continues at 2010
        CHECK(has(out, "\n2010  "));
        CHECK(has(m.execute("m"), "2014  "));  // continues
        m.execute("e 0400 08 05 0C 0C 0F");
        CHECK(has(m.execute("m.s 0400 5"), "HELLO"));
        m.execute("f 3000 300F AA 55");
        CHECK_EQ(ram(m, 0x3000, 4), std::string("AA55AA55"));
        CHECK_EQ(ram(m, 0x300E, 3), std::string("AA5500"));  // 'to' is inclusive
        CHECK(isError(m.execute("f 300F 3000 1")));
        CHECK_EQ(m.execute("c 3000 3003 3004"), std::string("identical\n"));
        m.execute("e 3005 11");
        out = m.execute("c 3000 3003 3004");
        CHECK(has(out, "3001: 55   3005: 11"));
        m.execute("t 3000 3007 3002");  // overlapping copy upwards
        CHECK_EQ(ram(m, 0x3002, 8), std::string("AA55AA55AA11AA55"));
        out = m.execute("h 2000 2FFF \"LL\"");
        CHECK_EQ(out, std::string("  2002\n"));
        CHECK_EQ(m.execute("h 2000 2FFF 99 98"), std::string("not found\n"));
        CHECK(has(m.execute("h 3000 30FF AA"), "  3000\n"));
        m.execute("f 0000 FFFF EA");  // whole memory
        CHECK_EQ(ram(m, 0xFFFF, 1), std::string("EA"));
        CHECK(has(m.execute("h 0000 FFFF EA"), "more"));
    }

    // ---- assemble mode, disassembler, labels ----------------------------------------------------------
    {
        Monitor m;
        CHECK_EQ(m.execute("a c000"), std::string());
        CHECK_EQ(m.prompt(), std::string("C000:"));
        CHECK_EQ(m.execute("start: LDX #$05"), std::string("  C000  A2 05     LDX #$05\n"));
        m.execute("       LDA #0");  // decimal in assemble mode
        m.execute("loop:");
        m.execute("  CLC");
        m.execute("  ADC #3");
        CHECK(isError(m.execute("  ADC #300")));  // error: nothing changes, still in assemble mode
        CHECK_EQ(m.prompt(), std::string("C007:"));
        CHECK(isError(m.execute("bogus")));       // a label without colon
        CHECK(isError(m.execute("BNE later")));   // no forward references
        m.execute("  DEX");
        m.execute("  BNE loop");
        m.execute("  STA $0400");
        m.execute("value = $FB");
        m.execute("  STA value");
        m.execute("  LAX ($10),Y");               // undocumented opcodes are allowed
        m.execute("  .BYTE 1,2");
        m.execute("  RTS");
        CHECK_EQ(m.execute(""), std::string());
        CHECK_EQ(m.prompt(), std::string("-"));
        CHECK_EQ(ram(m, 0xC000, 8), std::string("A205A900186903CA"));
        std::string out = m.execute("d start 9");
        CHECK(has(out, "start:\n  C000  A2 05     LDX #$05\n"));
        CHECK(has(out, "loop:\n  C004  18        CLC\n"));
        CHECK(has(out, "  C008  D0 FA     BNE loop\n"));
        CHECK(has(out, "  C00D  85 FB     STA value\n"));
        CHECK(has(out, "  C00F  B3 10     LAX ($10),Y\n"));
        CHECK(has(m.execute("d"), "RTS"));  // continues after the last line
        CHECK(isError(m.execute("d c000 0")));
        // the PC marker
        m.execute("r pc=c004");
        CHECK(has(m.execute("d c000 3"), "> C004  18"));
        // .BYTE for an unassigned opcode? All opcodes are instructions: JAM
        m.execute("e 4000 02");
        CHECK(has(m.execute("d 4000 1"), "JAM"));
        // symbols in the monitor's symbol table are known to later lines
        m.execute("sym later c100");
        m.execute("a c200");
        m.execute("JMP later");
        m.execute("");
        CHECK_EQ(ram(m, 0xC200, 3), std::string("4C00C1"));
    }

    // ---- registers --------------------------------------------------------------------------------------
    {
        Monitor m;
        std::string out = m.execute("r pc=c000 a=1 x=@a y=FF p=81 s=f0");
        CHECK(has(out, "PC=C000  A=01 X=01 Y=FF  S=F0  P=A1 N.-....C"));  // bit 5 is always 1
        CHECK(isError(m.execute("r a=100")));
        CHECK(isError(m.execute("r q=1")));
        CHECK(isError(m.execute("r a")));
        CHECK(isError(m.execute("r a=2 q=1")));  // all or nothing
        CHECK(has(m.execute("r"), "A=01"));
    }

    // ---- running: step, step over, until return, breakpoints, watchpoints, JAM, limit --------------------
    {
        Monitor m;
        CHECK(has(type(m, {"a c000", "main: LDX #3", "JSR sub"}), "? undefined symbol"));  // forward reference
        type(m, {"", "sym sub c100", "a c000", "main: LDX #3", "JSR sub", "JSR sub", "STA $0400", "JAM", "", "a sub", "INX",
                 "LDA #$42", "RTS", ""});
        CHECK_EQ(ram(m, 0xC002, 6), std::string("2000C12000C1"));
        m.execute("r pc=main");
        std::string out = m.execute("n");
        CHECK(has(out, "stopped: stepped  [1 instructions, 2 cycles]"));
        CHECK(has(out, "> C002  20 00 C1  JSR sub"));
        out = m.execute("o");  // runs the whole subroutine
        CHECK(has(out, "> C005  20 00 C1  JSR sub"));
        CHECK(has(out, "X=04"));
        out = m.execute("n 3");  // 2..20 steps are listed
        CHECK(has(out, "  C005  20 00 C1  JSR sub\n  C100  E8        INX\n  C101  A9 42     LDA #$42\n"));
        out = m.execute("u");
        CHECK(has(out, "stopped: returned"));
        CHECK(has(out, "> C008"));
        m.execute("w 0400");
        out = m.execute("g");
        CHECK(has(out, "stopped: watchpoint 0400 (instruction at C008)"));
        out = m.execute("g");
        CHECK(has(out, "stopped: JAM at C00B"));
        CHECK(has(out, "HALTED"));
        out = m.execute("n");
        CHECK(has(out, "JAM"));
        // breakpoints
        m.execute("b sub c008");
        out = m.execute("b");
        CHECK(has(out, "break C008\n"));
        CHECK(has(out, "break C100  (sub)"));
        CHECK(has(out, "watch 0400 len 1 w"));
        out = m.execute("g main");
        CHECK(has(out, "stopped: breakpoint at C100"));
        out = m.execute("g");  // continues past the breakpoint it is standing on
        CHECK(has(out, "stopped: breakpoint at C100"));
        m.execute("bc c100");
        CHECK(has(m.execute("g"), "breakpoint at C008"));
        m.execute("bc");
        CHECK_EQ(m.execute("b"), std::string("no breakpoints\n"));
        out = m.execute("ru c005");
        CHECK(has(out, "JAM"));  // C005 is behind the PC: runs into the JAM
        m.execute("r pc=main");
        CHECK(has(m.execute("ru c005"), "stopped: reached C005"));
        // instruction limit
        type(m, {"a d000", "spin: JMP spin", ""});
        m.execute("limit 10.");
        CHECK(has(m.execute("limit"), "(10)"));
        out = m.execute("g spin");
        CHECK(has(out, "instruction limit (10)  [10 instructions, 30 cycles]"));
        CHECK(isError(m.execute("limit 0")));
        // watch with length and read access
        type(m, {"a d100", "LDA $2005", "JAM", ""});
        m.execute("w 2000 10 r");
        CHECK(has(m.execute("g d100"), "watchpoint 2005"));
        m.execute("bc 2000");
        CHECK_EQ(m.execute("b"), std::string("no breakpoints\n"));
    }

    // ---- checkpoints and reset ----------------------------------------------------------------------------
    {
        Monitor m;
        m.execute("e 1000 11 22");
        m.execute("r a=5 pc=1234");
        CHECK_EQ(m.execute("ck one"), std::string());
        m.execute("e 1000 33");
        m.execute("r a=6");
        std::string out = m.execute("rb one");
        CHECK(has(out, "A=05"));
        CHECK_EQ(ram(m, 0x1000, 2), std::string("1122"));
        CHECK(isError(m.execute("rb two")));
        CHECK(isError(m.execute("ck")));
        m.execute("reset");
        CHECK_EQ(ram(m, 0x1000, 2), std::string("1122"));  // memory stays
        m.execute("reset mem");
        CHECK_EQ(ram(m, 0x1000, 2), std::string("0000"));
        CHECK(has(m.execute("rb one"), "PC=1234"));  // checkpoints survive a reset
    }

    // ---- interrupts on the generic machine --------------------------------------------------------------
    {
        Monitor m;
        type(m, {"e fffe 00 c0", "e fffa 00 c1", "a c000", "RTI", "", "a c100", "RTI", "", "a 2000", "NOP", "NOP", ""});
        m.execute("r pc=2000 p=24");
        CHECK(isError(m.execute("irq")));  // I = 1
        m.execute("r p=20");
        std::string out = m.execute("irq");
        CHECK(has(out, "IRQ taken, handler at C000"));
        CHECK(has(m.execute("n"), "PC=2000"));  // RTI returns, the line was released
        CHECK(has(m.execute("n"), "PC=2001"));
        out = m.execute("nmi");
        CHECK(has(out, "NMI taken, handler at C100"));
        m.execute("n");
        m.execute("irq on");
        m.execute("n");  // taken (I = 0)
        CHECK(has(m.execute("r"), "PC=C000"));
        m.execute("irq off");
        CHECK(isError(m.execute("irq maybe")));
    }

    // ---- files ------------------------------------------------------------------------------------------
    {
        const std::string src = tempFile("m6510_monitor_test.asm");
        const std::string prg = tempFile("m6510_monitor_test.prg");
        const std::string raw = tempFile("m6510_monitor_test.bin");
        const std::string sym = tempFile("m6510_monitor_test.sym");
        const std::string snap = tempFile("m6510_monitor_test.snap");
        writeTextFile(src, "* = $C000\nstart: LDA #1\n STA $D020\nloop: JMP loop\n");
        MonitorOptions locked;
        locked.allowFiles = false;
        Monitor nofiles(locked);
        CHECK(has(nofiles.execute("asm \"" + src + "\""), "disabled"));
        CHECK(has(nofiles.execute("l " + prg), "disabled"));

        Monitor m;
        std::string out = m.execute("asm " + src);
        CHECK(has(out, "ok: C000-C007 (8 bytes), 2 symbols"));
        CHECK(has(m.execute("d c000 1"), "start:"));
        CHECK(has(m.execute("sv " + prg + " c000 c007"), "PRG, load address C000"));
        CHECK_EQ(hexBytes(readBinaryFile(prg)).substr(0, 8), std::string("00 C0 A9"));
        m.execute("sv " + raw + " c000 c001 raw");
        CHECK_EQ(hexBytes(readBinaryFile(raw)), std::string("A9 01"));
        CHECK(has(m.execute("symsave " + sym), "2 symbols"));
        m.execute("reset mem");
        m.execute("sym -start");
        m.execute("sym -loop");
        out = m.execute("l " + prg);
        CHECK(has(out, "ok: C000-C007 (8 bytes), PC=C000"));
        CHECK_EQ(ram(m, 0xC000, 2), std::string("A901"));
        m.execute("l " + raw + " 2000");
        CHECK_EQ(ram(m, 0x2000, 2), std::string("A901"));
        CHECK(isError(m.execute("l " + raw)));  // raw needs an address
        m.execute("l " + prg + " 3000");        // the address overrides the load address
        CHECK_EQ(ram(m, 0x3000, 2), std::string("A901"));
        CHECK(has(m.execute("symload " + sym), "ok: 2 symbols"));
        CHECK(has(m.execute("d c005 1"), "loop:"));
        CHECK(isError(m.execute("l /nonexistent/file.prg")));
        writeTextFile(src, "LDA #1\n BOGUS\n");
        CHECK(has(m.execute("asm " + src), ":2: "));
        // snapshot files
        m.execute("r a=77 pc=c000");
        CHECK_EQ(m.execute("savestate " + snap), std::string("ok\n"));
        m.execute("reset mem");
        CHECK(has(m.execute("loadstate " + snap), "A=77"));
        CHECK_EQ(ram(m, 0xC000, 2), std::string("A901"));
        m.execute("machine c64");
        CHECK(isError(m.execute("loadstate " + snap)));  // generic snapshot, C64 machine
        CHECK(isError(m.execute("rom kernal " + raw)));  // wrong size
        CHECK(isError(m.execute("rom other " + raw)));
        for (const std::string& f : {src, prg, raw, sym, snap}) std::remove(f.c_str());
    }

    // ---- C64: machine, banking, KERNAL emulation, screen ----------------------------------------------
    {
        Monitor m;
        CHECK(has(m.execute("machine"), "generic"));
        CHECK(isError(m.execute("bank")));
        CHECK(isError(m.execute("screen")));
        CHECK(isError(m.execute("machine amiga")));
        CHECK(has(m.execute("machine c64"), "KERNAL emulated"));
        CHECK(has(m.execute("bank"), "$01=37"));
        std::string out = m.execute("bank 35");
        CHECK(has(out, "$E000:RAM"));
        m.execute("bank 37");
        CHECK(has(m.execute("bank"), "$E000:KERNAL"));
        // the KERNAL jump table is known by name
        type(m, {"sym text c100", "a c000", "LDX #0", "loop: LDA text,X", "BEQ $C00D", "JSR CHROUT", "INX", "BNE loop",
                 "INC $D020", "RTS", ""});
        m.execute("e c100 \"HI\" 0d 0");
        CHECK(has(m.execute("d c007 1"), "JSR CHROUT"));
        out = m.execute("run c000");
        CHECK(has(out, "stopped: program returned to BASIC"));
        CHECK_EQ(m.execute("console"), std::string("HI\n"));
        CHECK_EQ(m.execute("console"), std::string("(empty)\n"));
        out = m.execute("kernal");
        CHECK(has(out, "CHROUT  A=48"));
        CHECK(has(out, "call at C007"));
        CHECK_EQ(m.execute("kernal"), std::string("no KERNAL calls\n"));
        out = m.execute("io");
        CHECK(has(out, "D020 <- "));
        CHECK(has(out, "VIC EXTCOL"));
        CHECK(has(out, "by C00D"));
        CHECK_EQ(m.execute("io"), std::string("no writes\n"));
        out = m.execute("screen");
        CHECK(has(out, "|HI                                      |"));
        CHECK(has(out, "text, screen at 0400"));
        // reading I/O through the processor's view; writing below the KERNAL ROM
        m.execute("e e000 12");
        CHECK_EQ(ram(m, 0xE000, 1), std::string("12"));
        CHECK(!has(m.execute("m e000 1"), " 12 "));  // the ROM is visible
        m.execute("bank 35");
        CHECK(has(m.execute("m e000 1"), "E000  12"));
        m.execute("bank 37");
        // keys for GETIN
        type(m, {"a c200", "JSR GETIN", "STA $C300", "JSR GETIN", "STA $C301", "RTS", ""});
        m.execute("keys \"a\\n\"");
        m.execute("run c200");
        CHECK_EQ(ram(m, 0xC300, 2), std::string("410D"));
        CHECK(isError(m.execute("keys")));
        // run with a BASIC starter
        AssemblerOptions ao;
        const AssemblyResult r = assemble(".BASIC 10,start\nstart: INC $D021\n RTS\n", ao);
        CHECK(r.ok());
        for (const Segment& s : r.segments) m.machine().memory().load(s.address, s.bytes);
        out = m.execute("run");
        CHECK(has(out, "program returned to BASIC"));
        CHECK(has(m.execute("io"), "BGCOL0"));
        m.execute("e 0801 0 0");
        CHECK(isError(m.execute("run")));
        const std::string png = tempFile("m6510_monitor_test.png");
        CHECK(has(m.execute("shot " + png), "ok: 320 x 200 pixels, text, replacement character set"));
        CHECK_EQ(hexBytes(readBinaryFile(png)).substr(0, 11), std::string("89 50 4E 47"));
        CHECK(has(m.execute("shot " + png + " 0 0 2 1"), "ok: 128 x 64 pixels"));  // 2 x 1 cells, zoom 320/16 -> 8
        CHECK(has(m.execute("shot " + png + " 0 0 28 19 1 border"), "ok: 320 x 200"));
        CHECK(has(m.execute("shot " + png + " border pepto"), "ok: 384 x 272"));
        CHECK(has(m.execute("shot " + png + " 2"), "ok: 640 x 400"));
        CHECK(isError(m.execute("shot " + png + " 0 0 29 1")));  // wider than 40 cells
        CHECK(isError(m.execute("shot " + png + " 0 0")));
        CHECK(isError(m.execute("shot")));
        std::remove(png.c_str());
        // without the KERNAL emulation
        CHECK(has(m.execute("machine c64 rom"), "KERNAL missing"));
        CHECK(isError(m.execute("kernal")));
        CHECK(has(m.execute("io"), "no writes"));
        CHECK(isError(m.execute("machine c64 basic")));
        // C64 interrupts through the KERNAL's IRQ entry
        m.execute("machine c64");
        type(m, {"a c000", "CLI", "NOP", "NOP", ""});
        m.execute("r pc=c000");
        m.execute("n");
        CHECK(has(m.execute("irq"), "handler at FF48"));
        // a machine change clears the symbols
        m.execute("sym x1 1");
        m.execute("machine generic");
        CHECK_EQ(m.execute("sym"), std::string("no symbols\n"));
    }

    // ---- random command lines: never a crash or an exception ------------------------------------------
    {
        MonitorOptions opt;
        opt.allowFiles = false;
        opt.maxInstructions = 2000;
        Monitor m(opt);
        std::mt19937 rng(6510);
        const char* cmds[] = {"d", "m", "m.s", "e", "e.w", "f", "c", "t", "h", "a", "r", "n", "o", "u", "g", "ru", "b", "bc", "w",
                              "limit", "sym", "ck", "rb", "irq", "nmi", "reset", "machine", "bank", "io", "kernal", "console",
                              "screen", "keys", "run", "shot", "l", "sv", "asm", "symload", "?", "LDA", ""};
        const char* args[] = {"0", "1", "FF", "FFFF", "10000", "c000", "@pc", "@a+1", "\"AB\"", "\"", "c64", "generic", "rom",
                              "on", "off", "r", "rw", "-x", "x", "pc=1", "a=@x", "p=0", "$10", "100.", "1+", "*", ">1234",
                              "kernal", "mem", "NOP", "#1", "LDA #1", ":", "x:"};
        for (int i = 0; i < 4000; ++i) {
            std::string line = cmds[rng() % (sizeof(cmds) / sizeof(*cmds))];
            const int nargs = static_cast<int>(rng() % 4);
            for (int k = 0; k < nargs; ++k) line += std::string(" ") + args[rng() % (sizeof(args) / sizeof(*args))];
            std::string out;
            bool threw = false;
            try {
                out = m.execute(line);
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(out.empty() || out.back() == '\n');
            if (m.finished()) break;
        }
        while (m.prompt() != "-") m.execute("");
        CHECK_EQ(m.prompt(), std::string("-"));
    }

    // ---- the example script of docs/monitor.md -----------------------------------------------------------
    {
        Monitor m;
        const std::string out = type(m, {"a c000", "start: LDX #$05", "       LDA #$00", "loop:  CLC", "       ADC #$03", "       DEX",
                                         "       BNE loop", "       STA $0400", "done:  RTS", "", "d start 8", "b loop", "g start",
                                         "r", "bc", "ru done", "m 0400 1", "q"});
        CHECK(has(out, "stopped: breakpoint at C004  [2 instructions, 4 cycles]"));
        CHECK(has(out, "stopped: reached C00D"));
        CHECK(has(out, "0400  0F"));
        CHECK(m.finished());
    }

    return finish("monitor");
}
