#include "m6510/decoder.hpp"

#include <initializer_list>

#include "test_util.hpp"

using namespace m6510;

namespace {

Instruction at(std::initializer_list<int> bytes, Address address = 0x1000, CpuLevel level = CpuLevel::Stable,
               NumberStyle style = NumberStyle::Dollar) {
    Memory m;
    Address a = address;
    for (int b : bytes) m.write8(a++, static_cast<Byte>(b));
    DecodeOptions o;
    o.level = level;
    o.style = style;
    return decode(m, address, o);
}

std::string text(std::initializer_list<int> bytes, Address address = 0x1000, CpuLevel level = CpuLevel::Stable) {
    return at(bytes, address, level).text();
}

}  // namespace

int main() {
    // every addressing mode
    CHECK_EQ(text({0xEA}), std::string("NOP"));
    CHECK_EQ(text({0x0A}), std::string("ASL A"));
    CHECK_EQ(text({0xA9, 0x01}), std::string("LDA #$01"));
    CHECK_EQ(text({0xA5, 0xFB}), std::string("LDA $FB"));
    CHECK_EQ(text({0xB5, 0x10}), std::string("LDA $10,X"));
    CHECK_EQ(text({0xB6, 0x10}), std::string("LDX $10,Y"));
    CHECK_EQ(text({0x8D, 0x20, 0xD0}), std::string("STA $D020"));
    CHECK_EQ(text({0xBD, 0x00, 0x04}), std::string("LDA $0400,X"));
    CHECK_EQ(text({0xB9, 0x00, 0x04}), std::string("LDA $0400,Y"));
    CHECK_EQ(text({0x6C, 0x14, 0x03}), std::string("JMP ($0314)"));
    CHECK_EQ(text({0xA1, 0x20}), std::string("LDA ($20,X)"));
    CHECK_EQ(text({0x91, 0xFB}), std::string("STA ($FB),Y"));
    CHECK_EQ(text({0xD0, 0xFE}), std::string("BNE $1000"));
    CHECK_EQ(text({0xF0, 0x10}), std::string("BEQ $1012"));
    CHECK_EQ(text({0x20, 0xD2, 0xFF}), std::string("JSR $FFD2"));

    // lengths and bytes
    CHECK_EQ(at({0xEA}).length(), std::size_t{1});
    CHECK_EQ(at({0xA9, 0x01}).length(), std::size_t{2});
    CHECK_EQ(at({0x8D, 0x20, 0xD0}).length(), std::size_t{3});
    CHECK_EQ(hexBytes(at({0x8D, 0x20, 0xD0}).bytes), std::string("8D 20 D0"));

    // absolute with a zero-page value: forced with .W where a zero-page form exists
    CHECK_EQ(text({0xAD, 0x12, 0x00}), std::string("LDA.W $0012"));
    CHECK_EQ(text({0xBD, 0x12, 0x00}), std::string("LDA.W $0012,X"));
    CHECK_EQ(text({0xBE, 0x12, 0x00}), std::string("LDX.W $0012,Y"));
    CHECK_EQ(text({0xB9, 0x12, 0x00}), std::string("LDA $0012,Y"));  // there is no LDA zp,Y
    CHECK_EQ(text({0x99, 0x12, 0x00}), std::string("STA $0012,Y"));
    CHECK_EQ(text({0x4C, 0x12, 0x00}), std::string("JMP $0012"));  // no zero-page JMP
    CHECK_EQ(text({0x6C, 0x12, 0x00}), std::string("JMP ($0012)"));

    // flow and targets
    {
        const Instruction j = at({0x4C, 0x00, 0xC0});
        CHECK(j.flow == FlowKind::Jump);
        CHECK(j.target == std::optional<Address>(0xC000));
        const Instruction s = at({0x20, 0xD2, 0xFF});
        CHECK(s.flow == FlowKind::Call);
        CHECK(s.target == std::optional<Address>(0xFFD2));
        const Instruction b = at({0x90, 0x80});
        CHECK(b.flow == FlowKind::CondJump);
        CHECK(b.target == std::optional<Address>(0x1000 + 2 - 128));
        CHECK(at({0x60}).flow == FlowKind::Return);
        CHECK(at({0x40}).flow == FlowKind::Return);
        CHECK(at({0x00}).flow == FlowKind::Return);
        CHECK(at({0x6C, 0x14, 0x03}).flow == FlowKind::Jump);
        CHECK(!at({0x6C, 0x14, 0x03}).target.has_value());
        CHECK(at({0x6C, 0x14, 0x03}).reference == std::optional<Address>(0x0314));
        CHECK(at({0xA5, 0xFB}).reference == std::optional<Address>(0x00FB));
        CHECK(!at({0xA9, 0xFB}).reference.has_value());
        CHECK(at({0xEA}).flow == FlowKind::Normal);
        CHECK(at({0x02}, 0x1000, CpuLevel::All).flow == FlowKind::Halt);
    }

    // branch targets wrap at 64 KiB
    CHECK_EQ(text({0xD0, 0x10}, 0xFFF8), std::string("BNE $000A"));
    CHECK_EQ(text({0xD0, 0xF0}, 0x0004), std::string("BNE $FFF6"));
    // operands wrap at the end of memory
    {
        Memory m;
        m.write8(0xFFFF, 0xAD);
        m.write8(0x0000, 0x34);
        m.write8(0x0001, 0x12);
        CHECK_EQ(decode(m, 0xFFFF).text(), std::string("LDA $1234"));
    }

    // CPU levels
    CHECK_EQ(text({0xA7, 0x10}), std::string("LAX $10"));
    CHECK_EQ(text({0xA7, 0x10}, 0x1000, CpuLevel::Documented), std::string(".BYTE $A7"));
    CHECK(!at({0xA7, 0x10}, 0x1000, CpuLevel::Documented).valid);
    CHECK_EQ(at({0xA7, 0x10}, 0x1000, CpuLevel::Documented).length(), std::size_t{1});
    CHECK_EQ(text({0x8B, 0x10}), std::string(".BYTE $8B"));  // unstable: only with All
    CHECK_EQ(text({0x8B, 0x10}, 0x1000, CpuLevel::All), std::string("ANE #$10"));
    CHECK_EQ(text({0x02}), std::string(".BYTE $02"));
    CHECK_EQ(text({0x02}, 0x1000, CpuLevel::All), std::string("JAM"));
    CHECK_EQ(text({0xC7, 0x20}), std::string("DCP $20"));
    CHECK_EQ(text({0xEB, 0x01}), std::string("USBC #$01"));
    CHECK_EQ(text({0x0C, 0x34, 0x12}), std::string("NOP $1234"));
    CHECK_EQ(text({0x0C, 0x12, 0x00}), std::string("NOP.W $0012"));

    // canonical (preferred) opcodes
    CHECK(at({0xEA}).canonical);
    CHECK(!at({0x1A}).canonical);
    CHECK_EQ(text({0x1A}), std::string("NOP"));
    CHECK(!at({0x2B, 0x01}).canonical);
    CHECK_EQ(text({0x2B, 0x01}), std::string("ANC #$01"));

    // number styles
    CHECK_EQ(at({0xA9, 0xFF}, 0x1000, CpuLevel::Stable, NumberStyle::CPrefix).text(), std::string("LDA #0xFF"));
    CHECK_EQ(at({0x8D, 0x20, 0xD0}, 0x1000, CpuLevel::Stable, NumberStyle::HSuffix).text(), std::string("STA 0D020H"));

    // symbolizer
    {
        Memory m;
        m.load(0x1000, std::vector<Byte>{0x20, 0xD2, 0xFF, 0xAD, 0x12, 0x00, 0xA9, 0x12, 0xD0, 0xF9});
        DecodeOptions o;
        o.symbolizer = [](Address a) -> std::string {
            if (a == 0xFFD2) return "CHROUT";
            if (a == 0x0012) return "zp_var";
            if (a == 0x1003) return "here";
            return "";
        };
        CHECK_EQ(decode(m, 0x1000, o).text(), std::string("JSR CHROUT"));
        CHECK_EQ(decode(m, 0x1003, o).text(), std::string("LDA.W zp_var"));
        CHECK_EQ(decode(m, 0x1006, o).text(), std::string("LDA #$12"));  // immediates are never names
        CHECK_EQ(decode(m, 0x1008, o).text(), std::string("BNE here"));
    }

    CHECK(hasZeroPageForm("LDA", Mode::Absolute));
    CHECK(!hasZeroPageForm("LDA", Mode::AbsoluteY));
    CHECK(hasZeroPageForm("LDX", Mode::AbsoluteY));
    CHECK(!hasZeroPageForm("JMP", Mode::Absolute));

    return finish("decoder");
}
