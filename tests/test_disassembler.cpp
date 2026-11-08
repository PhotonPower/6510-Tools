#include "m6510/disassembler.hpp"

#include <random>

#include "m6510/assembler.hpp"
#include "m6510/petscii.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

Memory memoryFrom(const std::string& src, AssemblyResult& r, CpuLevel level = CpuLevel::Stable) {
    AssemblerOptions o;
    o.origin = 0x1000;
    o.level = level;
    r = assemble(src, o);
    for (const Diagnostic& d : r.errors) std::cerr << "  test program, line " << d.line << ": " << d.message << "\n";
    Memory m;
    for (const Segment& s : r.segments) m.load(s.address, s.bytes);
    return m;
}

bool contains(const std::string& text, const std::string& part) {
    if (text.find(part) != std::string::npos) return true;
    std::cerr << "  expected to find '" << part << "' in:\n" << text << "\n";
    return false;
}

// Source output of [start, end) re-assembles to the same bytes.
bool sourceRoundTrip(const Memory& m, Address start, std::uint32_t end, const Disassembler& dis, CpuLevel level) {
    DisassemblerOptions o;
    o.asmSource = true;
    const std::string src = dis.render(o);
    AssemblerOptions ao;
    ao.level = level;
    const AssemblyResult r = assemble(src, ao);
    const std::vector<Byte> expect = m.readBytes(start, end - start);
    if (r.ok() && r.lowest() == start && r.flatten() == expect) return true;
    std::cerr << "  source round trip failed";
    if (!r.ok()) std::cerr << ": line " << r.errors.front().line << ": " << r.errors.front().message;
    std::cerr << "\n" << src.substr(0, 3000) << "\n";
    return false;
}

}  // namespace

int main() {
    // ---- flow analysis separates code from data -----------------------------------------------
    {
        AssemblyResult r;
        const Memory m = memoryFrom(
            "start: LDX #0\n"
            "next:  LDA msg,X\n"
            "       BEQ done\n"
            "       JSR $FFD2\n"
            "       INX\n"
            "       BNE next\n"
            "done:  RTS\n"
            "msg:   .BYTE \"HELLO\",13,0\n",
            r);
        Disassembler dis(m, 0x1000, r.end());
        for (const auto& [a, n] : kernalSymbols()) dis.addSymbol(a, n);
        dis.addComment(0x1000, "entry");
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        CHECK_EQ(dis.codeBytes(), std::size_t{14});
        const std::string text = dis.render();
        CHECK(contains(text, "CHROUT = $FFD2"));
        CHECK(contains(text, "JSR CHROUT"));
        CHECK(contains(text, "LDA L_100E,X"));
        CHECK(contains(text, "BNE L_1002"));
        CHECK(contains(text, ".BYTE \"HELLO\",$0D,$00"));
        CHECK(contains(text, "; entry"));
        CHECK(contains(text, "; <- $100B"));  // xref of the loop label
        CHECK(sourceRoundTrip(m, 0x1000, r.end(), dis, CpuLevel::Stable));
    }

    // ---- a path ends at RTS/JMP/RTI/BRK; data behind it stays data --------------------------
    {
        AssemblyResult r;
        const Memory m = memoryFrom(" JMP over\n .BYTE $A9,$01,$60\nover: RTS\n .BYTE $EA,$EA", r);
        Disassembler dis(m, 0x1000, r.end());
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, ".BYTE $A9,$01,$60"));
        CHECK(contains(text, ".BYTE $EA,$EA"));
        CHECK_EQ(dis.codeBytes(), std::size_t{4});
    }

    // ---- self-modifying code: label in the middle of an instruction -------------------------
    {
        AssemblyResult r;
        const Memory m = memoryFrom("start: LDA #5\n STA patch+1\npatch: LDX #0\n RTS", r);
        Disassembler dis(m, 0x1000, r.end());
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, "STA L_1005+1"));
        CHECK(contains(text, "L_1005:"));
        CHECK(sourceRoundTrip(m, 0x1000, r.end(), dis, CpuLevel::Stable));
    }

    // ---- zero-page value in an absolute instruction keeps .W ------------------------------------
    {
        AssemblyResult r;
        const Memory m = memoryFrom(" LDA.W $0012\n STA.W $00FB,X\n RTS", r);
        Disassembler dis(m, 0x1000, r.end());
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, "LDA.W $0012"));
        CHECK(contains(text, "STA.W $00FB,X"));
        CHECK(sourceRoundTrip(m, 0x1000, r.end(), dis, CpuLevel::Stable));
    }

    // ---- non-preferred opcodes are kept as .BYTE in source mode ------------------------------
    {
        Memory m;
        m.load(0x2000, std::vector<Byte>{0x1A, 0x80, 0x12, 0x2B, 0x05, 0x60});
        Disassembler dis(m, 0x2000, 0x2006);
        dis.analyzeFlow();
        CHECK(contains(dis.render(), "NOP"));
        DisassemblerOptions o;
        o.asmSource = true;
        CHECK(contains(dis.render(o), ".BYTE $1A"));
        CHECK(contains(dis.render(o), ".BYTE $2B,$05"));
        CHECK(sourceRoundTrip(m, 0x2000, 0x2006, dis, CpuLevel::Stable));
    }

    // ---- CPU level: undocumented opcodes end a path at level Documented ---------------------
    {
        Memory m;
        m.load(0x2000, std::vector<Byte>{0xA7, 0x10, 0x60});
        Disassembler doc(m, 0x2000, 0x2003, CpuLevel::Documented);
        doc.analyzeFlow();
        CHECK_EQ(doc.codeBytes(), std::size_t{0});
        Disassembler st(m, 0x2000, 0x2003, CpuLevel::Stable);
        st.analyzeFlow();
        CHECK_EQ(st.codeBytes(), std::size_t{3});
        CHECK(contains(st.render(), "LAX $10"));
    }

    // ---- BASIC starter ------------------------------------------------------------------------
    {
        AssemblerOptions o;
        const AssemblyResult r = assemble(" * = $0801\n .BASIC\nstart: INC $D020\n JMP start", o);
        Memory m;
        for (const Segment& s : r.segments) m.load(s.address, s.bytes);
        Disassembler dis(m, 0x0801, r.end());
        dis.analyzeFlow();  // no entry given: the SYS address
        CHECK(dis.entries().count(0x080D) == 1);
        const std::string text = dis.render();
        CHECK(contains(text, ".BASIC 10,L_080D"));
        CHECK(contains(text, "10 SYS2061"));
        CHECK(contains(text, "INC $D020"));
        CHECK(sourceRoundTrip(m, 0x0801, r.end(), dis, CpuLevel::Stable));
    }

    // ---- comment file and symbol files --------------------------------------------------------
    {
        Memory m;
        m.load(0x3000, std::vector<Byte>{0xEA, 0x60, 0xEA, 0x60});
        Disassembler dis(m, 0x3000, 0x3004);
        const auto diags = loadCommentFile("; comment\n$3000,\"first\"\n12290,\"second\"\nbad line\n$zz,\"x\"\n", dis);
        CHECK_EQ(diags.size(), std::size_t{2});
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, "; first"));
        CHECK(contains(text, "; second"));  // 12290 = $3002 is an entry point
        CHECK_EQ(dis.codeBytes(), std::size_t{4});

        std::map<Address, std::string> syms;
        const auto d2 = parseSymbolFile("start = $1000\nal C:080d .entry\n; c\nbroken\n", syms);
        CHECK_EQ(d2.size(), std::size_t{1});
        CHECK_EQ(syms[0x1000], std::string("start"));
        CHECK_EQ(syms[0x080D], std::string("entry"));
    }

    // ---- linear mode --------------------------------------------------------------------------
    {
        Memory m;
        m.load(0x4000, std::vector<Byte>{0x60, 0xA9, 0x01, 0x02, 0xEA});
        Disassembler dis(m, 0x4000, 0x4005);
        dis.analyzeLinear();
        const std::string text = dis.render();
        CHECK(contains(text, "LDA #$01"));
        CHECK(contains(text, ".BYTE $02"));  // JAM is not an instruction at the default level
        CHECK_EQ(dis.codeBytes(), std::size_t{4});
        // an instruction that would run past the end of the range is data
        Disassembler cut(m, 0x4000, 0x4002);
        cut.analyzeLinear();
        CHECK_EQ(cut.codeBytes(), std::size_t{1});
    }

    // ---- random memory: source output always re-assembles to the same bytes -------------------
    {
        std::mt19937 rng(64);
        int failures = 0;
        for (int round = 0; round < 300 && failures < 3; ++round) {
            Memory m;
            const Address start = static_cast<Address>(0x0800 + (rng() % 0x8000));
            const auto len = static_cast<std::uint32_t>(16 + rng() % 400);
            for (std::uint32_t i = 0; i < len; ++i) m.write8(static_cast<Address>(start + i), static_cast<Byte>(rng()));
            const CpuLevel level = static_cast<CpuLevel>(round % 3);
            Disassembler dis(m, start, start + len, level);
            for (const auto& [a, n] : kernalSymbols()) dis.addSymbol(a, n);
            if (round % 2 == 0) {
                dis.analyzeLinear();
            } else {
                for (int e = 0; e < 4; ++e) dis.addEntry(static_cast<Address>(start + rng() % len));
                dis.analyzeFlow();
            }
            const bool ok = sourceRoundTrip(m, start, start + len, dis, level);
            CHECK(ok);
            if (!ok) ++failures;
        }
    }

    return finish("disassembler");
}
