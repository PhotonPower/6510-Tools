#include "m6510/assembler.hpp"

#include <random>

#include "m6510/petscii.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

AssemblyResult run(const std::string& src, CpuLevel level = CpuLevel::Stable, Address origin = 0x1000) {
    AssemblerOptions o;
    o.origin = origin;
    o.level = level;
    return assemble(src, o);
}

// Bytes of a successful run; prints the errors otherwise.
std::string bytes(const std::string& src, CpuLevel level = CpuLevel::Stable) {
    const AssemblyResult r = run(src, level);
    for (const Diagnostic& d : r.errors) std::cerr << "  [" << src << "] line " << d.line << ": " << d.message << "\n";
    return r.ok() ? hexBytes(r.flatten()) : "ERROR";
}

// The first error message of a run, or "" if it succeeded.
std::string firstError(const std::string& src, CpuLevel level = CpuLevel::Stable) {
    const AssemblyResult r = run(src, level);
    return r.errors.empty() ? "" : r.errors.front().message;
}

bool fails(const std::string& src, CpuLevel level = CpuLevel::Stable) { return !run(src, level).ok(); }

}  // namespace

int main() {
    // ---- single instructions (well-known encodings) ------------------------------------------
    struct Case {
        const char* src;
        const char* bytes;
    };
    const Case cases[] = {
        {" NOP", "EA"},
        {" BRK", "00"},
        {" RTS", "60"},
        {" RTI", "40"},
        {" ASL", "0A"},
        {" asl a", "0A"},
        {" ROR A", "6A"},
        {" LDA #$01", "A9 01"},
        {" LDA #1", "A9 01"},
        {" LDA #-1", "A9 FF"},
        {" LDA #'A'", "A9 41"},
        {" LDA #<$1234", "A9 34"},
        {" LDA #>$1234", "A9 12"},
        {" LDA $FB", "A5 FB"},
        {" LDA $10,X", "B5 10"},
        {" LDX $10,Y", "B6 10"},
        {" STX $10,Y", "96 10"},
        {" LDA $D020", "AD 20 D0"},
        {" LDA $0400,X", "BD 00 04"},
        {" LDA $0400,Y", "B9 00 04"},
        {" LDA $12,Y", "B9 12 00"},  // no LDA zp,Y: absolute
        {" LDA ($20,X)", "A1 20"},
        {" LDA ($FB),Y", "B1 FB"},
        {" lda ( $fb ) , y", "B1 FB"},
        {" STA ($FB),Y", "91 FB"},
        {" JMP $C000", "4C 00 C0"},
        {" JMP ($0314)", "6C 14 03"},
        {" JMP ($12)", "6C 12 00"},
        {" JSR $FFD2", "20 D2 FF"},
        {" BIT $D011", "2C 11 D0"},
        {" BIT $24", "24 24"},
        {" INC $D019", "EE 19 D0"},
        {" DEC $02,X", "D6 02"},
        {" CPX #$28", "E0 28"},
        {" CPY $0400", "CC 00 04"},
        {" STY $FE,X", "94 FE"},
        {" SEI", "78"},
        {" CLI", "58"},
        {" TXS", "9A"},
        {" PLA", "68"},
        {" SBC #$01", "E9 01"},
        {" ADC ($02),Y", "71 02"},
        {" ORA $DC01", "0D 01 DC"},
        // forced forms
        {" LDA.W $12", "AD 12 00"},
        {" LDA.W $12,X", "BD 12 00"},
        {" LDX.W $12,Y", "BE 12 00"},
        {" LDA.B $12", "A5 12"},
        {" lda.w $12", "AD 12 00"},
        // branches
        {" BNE *", "D0 FE"},
        {" BEQ *+2", "F0 00"},
        {" BCC *+129", "90 7F"},
        {" BCS *-126", "B0 80"},
        // undocumented (stable)
        {" LAX $10", "A7 10"},
        {" LAX $1234,Y", "BF 34 12"},
        {" LAX ($10),Y", "B3 10"},
        {" SAX $10", "87 10"},
        {" SAX $10,Y", "97 10"},
        {" DCP $D019", "CF 19 D0"},
        {" DCM $20", "C7 20"},
        {" ISC ($20,X)", "E3 20"},
        {" ISB $20", "E7 20"},
        {" SLO $1234,X", "1F 34 12"},
        {" RLA $20", "27 20"},
        {" SRE $20,X", "57 20"},
        {" RRA $1234,Y", "7B 34 12"},
        {" ANC #$01", "0B 01"},
        {" ALR #$FE", "4B FE"},
        {" ASR #$FE", "4B FE"},
        {" ARR #$7F", "6B 7F"},
        {" SBX #$01", "CB 01"},
        {" USBC #$01", "EB 01"},
        {" NOP #$00", "80 00"},
        {" NOP $10", "04 10"},
        {" NOP $10,X", "14 10"},
        {" NOP $1234", "0C 34 12"},
        {" NOP $1234,X", "1C 34 12"},
    };
    for (const Case& c : cases) CHECK_EQ(bytes(c.src), std::string(c.bytes));

    // ---- CPU levels ---------------------------------------------------------------------------
    CHECK(fails(" LAX $10", CpuLevel::Documented));
    CHECK(firstError(" LAX $10", CpuLevel::Documented).find("undocumented") != std::string::npos);
    CHECK(fails(" ANE #$10"));
    CHECK_EQ(bytes(" ANE #$10", CpuLevel::All), std::string("8B 10"));
    CHECK_EQ(bytes(" XAA #$10", CpuLevel::All), std::string("8B 10"));
    CHECK_EQ(bytes(" LXA #$10", CpuLevel::All), std::string("AB 10"));
    CHECK_EQ(bytes(" SHA $1234,Y", CpuLevel::All), std::string("9F 34 12"));
    CHECK_EQ(bytes(" SHA ($10),Y", CpuLevel::All), std::string("93 10"));
    CHECK_EQ(bytes(" SHX $1234,Y", CpuLevel::All), std::string("9E 34 12"));
    CHECK_EQ(bytes(" SHY $1234,X", CpuLevel::All), std::string("9C 34 12"));
    CHECK_EQ(bytes(" TAS $1234,Y", CpuLevel::All), std::string("9B 34 12"));
    CHECK_EQ(bytes(" LAS $1234,Y", CpuLevel::All), std::string("BB 34 12"));
    CHECK_EQ(bytes(" JAM", CpuLevel::All), std::string("02"));
    CHECK(fails(" JAM"));
    CHECK_EQ(bytes(" .CPU 6510X\n JAM"), std::string("02"));
    CHECK(fails(" .CPU 6502\n LAX $10"));
    CHECK_EQ(bytes(" .CPU 6502\n LDA $10\n .CPU 6510\n LAX $10"), std::string("A5 10 A7 10"));
    CHECK(fails(" .CPU Z80"));

    // ---- labels, zero page / absolute selection, forward references ---------------------------
    CHECK_EQ(bytes("ptr = $FB\n LDA (ptr),Y\n STA ptr+1"), std::string("B1 FB 85 FC"));
    CHECK_EQ(bytes(" LDA var\n RTS\nvar = $10"), std::string("A5 10 60"));  // forward reference into the zero page
    CHECK_EQ(bytes(" LDA data\n RTS\ndata .BYTE 1"), std::string("AD 04 10 60 01"));  // forward reference: grows to absolute
    CHECK_EQ(bytes(" LDA data,X\n LDA data,Y\n RTS\ndata .BYTE 1"), std::string("BD 07 10 B9 07 10 60 01"));
    CHECK_EQ(bytes("loop: DEX\n BNE loop\n RTS"), std::string("CA D0 FD 60"));
    CHECK_EQ(bytes(" BNE done\n NOP\ndone: RTS"), std::string("D0 01 EA 60"));
    CHECK_EQ(bytes("start\n NOP\n JMP start"), std::string("EA 4C 00 10"));  // label in column 1 without colon
    CHECK_EQ(bytes("NOP\nRTS"), std::string("EA 60"));                     // a mnemonic in column 1 is an instruction
    CHECK_EQ(bytes("  lbl: nop\n jmp lbl"), std::string("EA 4C 00 10"));   // indented label with colon
    // local labels
    CHECK_EQ(bytes("a1: LDX #2\n.l: DEX\n BNE .l\nb1: LDY #2\n.l: DEY\n BNE .l"),
             std::string("A2 02 CA D0 FD A0 02 88 D0 FD"));
    {
        const AssemblyResult r = run("a1: NOP\n.l: NOP\n");
        CHECK(r.symbols.count("a1.l") == 1);
    }
    // a chain of forward references converges
    CHECK_EQ(bytes(" LDA va\n LDA vb\n RTS\nva = vb\nvb = $20"), std::string("A5 20 A5 20 60"));
    CHECK_EQ(bytes(" JMP far\n .FILL 300\nfar: RTS").substr(0, 8), std::string("4C 2F 11"));

    // ---- pseudo instructions -----------------------------------------------------------------
    CHECK_EQ(bytes(" .BYTE 1,2,$FF,-1"), std::string("01 02 FF FF"));
    CHECK_EQ(bytes(" DB 1\n DFB 2\n .BYT 3"), std::string("01 02 03"));
    CHECK_EQ(bytes(" .BYTE \"AB\",0"), std::string("41 42 00"));
    CHECK_EQ(bytes(" .TEXT \"a;b\""), std::string("61 3B 62"));  // ';' inside a string is no comment
    CHECK_EQ(bytes(" .BYTE \"a,b\""), std::string("61 2C 62"));
    CHECK_EQ(bytes(" .NULL \"HI\""), std::string("48 49 00"));
    CHECK_EQ(bytes(" .WORD $1234,1"), std::string("34 12 01 00"));
    CHECK_EQ(bytes(" DW $1234"), std::string("34 12"));
    CHECK_EQ(bytes(" .DBYTE $1234"), std::string("12 34"));
    CHECK_EQ(bytes(" .PETSCII \"Hello 1!\""), std::string("C8 45 4C 4C 4F 20 31 21"));
    CHECK_EQ(bytes(" .SCREEN \"@hello AZ\""), std::string("00 08 05 0C 0C 0F 20 41 5A"));
    CHECK_EQ(bytes(" .LO $1234,$5678\n .HI $1234,$5678"), std::string("34 78 12 56"));
    CHECK_EQ(bytes(" .LOHI $1234,$5678"), std::string("34 78 12 56"));
    CHECK_EQ(bytes(" .FILL 3"), std::string("00 00 00"));
    CHECK_EQ(bytes(" .FILL 3,$EA"), std::string("EA EA EA"));
    CHECK_EQ(bytes(" DS 2\n .DS 1,7\n .RES 1"), std::string("00 00 07 00"));
    CHECK_EQ(bytes(" NOP\n .ALIGN 4\n RTS"), std::string("EA 00 00 00 60"));
    CHECK_EQ(bytes(" .ALIGN 4,$FF\n RTS"), std::string("60"));  // already aligned at $1000
    CHECK_EQ(bytes("k EQU 5\n LDA #k"), std::string("A9 05"));
    CHECK_EQ(bytes("k=5\n LDA #k"), std::string("A9 05"));
    CHECK_EQ(bytes(" NOP\n END\n garbage here"), std::string("EA"));
    {
        const AssemblyResult r = run(" * = $C000\n NOP\n*=$C010\n RTS\n ORG $C020\n BRK");
        CHECK(r.ok());
        CHECK_EQ(r.segments.size(), std::size_t{3});
        CHECK_EQ(r.lowest(), Address{0xC000});
        CHECK_EQ(r.end(), std::uint32_t{0xC021});
        CHECK_EQ(r.flatten().size(), std::size_t{0x21});
        CHECK_EQ(r.flatten()[0x10], Byte{0x60});
    }
    CHECK_EQ(bytes("* this is a comment\n NOP ; and this"), std::string("EA"));

    // .BASIC
    {
        const AssemblyResult r = run(" * = $0801\n .BASIC\nstart: RTS");
        CHECK(r.ok());
        CHECK_EQ(hexBytes(r.flatten()), std::string("0B 08 0A 00 9E 32 30 36 31 00 00 00 60"));  // 10 SYS2061
        CHECK_EQ(r.symbols.at("start"), std::int64_t{2061});
        const AssemblyResult r2 = run(" * = $0801\n .BASIC 2026,go\n NOP\ngo: RTS");
        CHECK(r2.ok());
        CHECK(r2.flatten() == [] {
            std::vector<Byte> v = makeBasicStub(0x0801, 2026, 2062);
            v.push_back(0xEA);
            v.push_back(0x60);
            return v;
        }());
        // 3-digit and 5-digit targets
        CHECK_EQ(run(" * = $0100\n .BASIC\ngo: RTS").symbols.at("go"), std::int64_t{0x0100 + 11});
        CHECK_EQ(run(" * = $C000\n .BASIC\ngo: RTS").symbols.at("go"), std::int64_t{0xC000 + 13});
    }

    // ---- listing and symbol output -----------------------------------------------------------
    {
        const AssemblyResult r = run("start: LDA #1 ; go\n RTS");
        const std::string l = r.formatListing();
        CHECK(l.find("1000  A9 01") != std::string::npos);
        CHECK(l.find("start: LDA #1 ; go") != std::string::npos);
        CHECK_EQ(r.formatSymbols(), std::string("start = $1000\n"));
        CHECK_EQ(r.formatViceLabels(), std::string("al C:1000 .start\n"));
    }

    // ---- errors ------------------------------------------------------------------------------
    CHECK(fails(" LDA"));
    CHECK(fails(" INX #1"));
    CHECK(fails(" STA #1"));
    CHECK(fails(" LDA #256"));
    CHECK(fails(" LDA #-129"));
    CHECK(fails(" LDA ($1234),Y"));
    CHECK(fails(" LDA ($100,X)"));
    CHECK(fails(" JMP ($10000)"));
    CHECK(fails(" LDA $10000"));
    CHECK(fails(" STX $1234,Y"));  // STX abs,Y does not exist
    CHECK(fails(" LDA.B $1234"));
    CHECK(fails(" LDA.W #1"));
    CHECK(fails(" BNE *+130"));
    CHECK(firstError(" BNE *+130").find("too far") != std::string::npos);
    CHECK(fails(" LDA undefined_symbol"));
    CHECK(firstError(" LDA undefined_symbol").find("undefined") != std::string::npos);
    CHECK(fails("x: NOP\nx: NOP"));
    CHECK(fails("A: NOP"));      // register names (any case)
    CHECK(fails("x = 5"));
    CHECK(fails("lda: NOP"));    // mnemonic
    CHECK(fails(".l: NOP"));     // local label without a global one
    CHECK(fails(" FOO"));
    CHECK(fails(" LDA A"));
    CHECK(fails(" .BYTE 256"));
    CHECK(fails(" .WORD \"ab\""));
    CHECK(fails(" .PETSCII \"{\""));
    CHECK(fails(" * = $FFFF\n NOP\n NOP"));
    CHECK(fails(" EQU 5"));
    // all errors are collected, with line numbers
    {
        const AssemblyResult r = run(" NOP\n FOO\n LDA #300\n RTS");
        CHECK_EQ(r.errors.size(), std::size_t{2});
        CHECK_EQ(r.errors[0].line, 2);
        CHECK_EQ(r.errors[1].line, 3);
    }

    // ---- robustness: random source text never crashes ---------------------------------------
    {
        std::mt19937 rng(1234);
        const std::string alphabet = " \t#$%()*+,-./0123456789:;<=>ABCDEFXYZabcdefxyz\"'_[]\nLDASTAJMPBNE.BYTE";
        for (int i = 0; i < 3000; ++i) {
            std::string src;
            const int len = static_cast<int>(rng() % 80);
            for (int k = 0; k < len; ++k) src += alphabet[rng() % alphabet.size()];
            (void)run(src, CpuLevel::All);
        }
        CHECK(true);
    }

    return finish("assembler");
}
