#include "m6510/opcodes.hpp"

#include <map>
#include <set>

#include "test_util.hpp"

using namespace m6510;

int main() {
    const auto& t = opcodeTable();

    // counts from docs/instruction-set.md
    int documented = 0, illegal = 0, unstable = 0, jam = 0;
    std::set<std::string> docMnemonics;
    for (const OpInfo& op : t) {
        switch (op.kind) {
            case OpKind::Documented:
                ++documented;
                docMnemonics.insert(op.mnemonic);
                break;
            case OpKind::Illegal: ++illegal; break;
            case OpKind::Unstable: ++unstable; break;
            case OpKind::Jam: ++jam; break;
        }
    }
    CHECK_EQ(documented, 151);
    CHECK_EQ(docMnemonics.size(), std::size_t{56});
    CHECK_EQ(jam, 12);
    CHECK_EQ(unstable, 8);  // ANE, LXA, SHA x2, SHX, SHY, TAS, LAS
    CHECK_EQ(illegal + unstable + jam, 105);

    // JAM opcodes
    for (int op : {0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2}) {
        CHECK(opInfo(static_cast<Byte>(op)).kind == OpKind::Jam);
    }
    // the "11" column (cc = 11) is undocumented throughout
    for (int op = 3; op < 256; op += 4) CHECK(opInfo(static_cast<Byte>(op)).kind != OpKind::Documented);

    // 27 undocumented NOPs besides $EA
    int nops = 0;
    for (int op = 0; op < 256; ++op)
        if (std::string(t[static_cast<std::size_t>(op)].mnemonic) == "NOP" && op != 0xEA) ++nops;
    CHECK_EQ(nops, 27);

    // spot checks: mnemonic, mode, cycles, page penalty
    struct Spot {
        int op;
        const char* m;
        Mode mode;
        int cycles;
        bool page;
    };
    const Spot spots[] = {
        {0x00, "BRK", Mode::Implied, 7, false},     {0xA9, "LDA", Mode::Immediate, 2, false},
        {0xA5, "LDA", Mode::ZeroPage, 3, false},    {0xB5, "LDA", Mode::ZeroPageX, 4, false},
        {0xAD, "LDA", Mode::Absolute, 4, false},    {0xBD, "LDA", Mode::AbsoluteX, 4, true},
        {0xB9, "LDA", Mode::AbsoluteY, 4, true},    {0xA1, "LDA", Mode::IndirectX, 6, false},
        {0xB1, "LDA", Mode::IndirectY, 5, true},    {0x9D, "STA", Mode::AbsoluteX, 5, false},
        {0x91, "STA", Mode::IndirectY, 6, false},   {0x6C, "JMP", Mode::Indirect, 5, false},
        {0x4C, "JMP", Mode::Absolute, 3, false},    {0x20, "JSR", Mode::Absolute, 6, false},
        {0x60, "RTS", Mode::Implied, 6, false},     {0x40, "RTI", Mode::Implied, 6, false},
        {0xD0, "BNE", Mode::Relative, 2, true},     {0x0A, "ASL", Mode::Accumulator, 2, false},
        {0xFE, "INC", Mode::AbsoluteX, 7, false},   {0x96, "STX", Mode::ZeroPageY, 4, false},
        {0xB6, "LDX", Mode::ZeroPageY, 4, false},   {0xBE, "LDX", Mode::AbsoluteY, 4, true},
        {0xA7, "LAX", Mode::ZeroPage, 3, false},    {0xB3, "LAX", Mode::IndirectY, 5, true},
        {0x87, "SAX", Mode::ZeroPage, 3, false},    {0xC7, "DCP", Mode::ZeroPage, 5, false},
        {0xDB, "DCP", Mode::AbsoluteY, 7, false},   {0xE3, "ISC", Mode::IndirectX, 8, false},
        {0x0B, "ANC", Mode::Immediate, 2, false},   {0x4B, "ALR", Mode::Immediate, 2, false},
        {0x6B, "ARR", Mode::Immediate, 2, false},   {0xCB, "SBX", Mode::Immediate, 2, false},
        {0xEB, "USBC", Mode::Immediate, 2, false},  {0x8B, "ANE", Mode::Immediate, 2, false},
        {0xAB, "LXA", Mode::Immediate, 2, false},   {0x9B, "TAS", Mode::AbsoluteY, 5, false},
        {0xBB, "LAS", Mode::AbsoluteY, 4, true},    {0x9C, "SHY", Mode::AbsoluteX, 5, false},
        {0x9E, "SHX", Mode::AbsoluteY, 5, false},   {0x93, "SHA", Mode::IndirectY, 6, false},
        {0x1C, "NOP", Mode::AbsoluteX, 4, true},    {0x80, "NOP", Mode::Immediate, 2, false},
    };
    for (const Spot& s : spots) {
        const OpInfo& op = opInfo(static_cast<Byte>(s.op));
        CHECK_EQ(std::string(op.mnemonic), std::string(s.m));
        CHECK(op.mode == s.mode);
        CHECK_EQ(int{op.cycles}, s.cycles);
        CHECK_EQ(op.pagePenalty, s.page);
    }

    // documented opcodes never collide on (mnemonic, mode); the preferred form is unique
    std::map<std::pair<std::string, Mode>, int> preferredCount;
    for (const OpInfo& op : t)
        if (op.preferred) ++preferredCount[{op.mnemonic, op.mode}];
    for (const auto& [key, n] : preferredCount) CHECK_EQ(n, 1);
    for (const OpInfo& op : t)
        if (op.kind == OpKind::Documented) CHECK(op.preferred);
    CHECK(opInfo(0xEA).preferred);
    CHECK(!opInfo(0x1A).preferred);
    CHECK(opInfo(0x80).preferred);
    CHECK(!opInfo(0x89).preferred);
    CHECK(opInfo(0x0B).preferred);
    CHECK(!opInfo(0x2B).preferred);
    CHECK(opInfo(0x02).preferred);
    CHECK(!opInfo(0x12).preferred);

    // lookups
    CHECK(findOpcode("LDA", Mode::Immediate) == std::optional<Byte>(0xA9));
    CHECK(findOpcode("NOP", Mode::Implied) == std::optional<Byte>(0xEA));
    CHECK(!findOpcode("LDA", Mode::ZeroPageY).has_value());
    CHECK(!findOpcode("STA", Mode::Immediate).has_value());
    CHECK_EQ(canonicalMnemonic("dcm"), std::string("DCP"));
    CHECK_EQ(canonicalMnemonic("isb"), std::string("ISC"));
    CHECK_EQ(canonicalMnemonic("lda"), std::string("LDA"));
    CHECK(isMnemonic("lax"));
    CHECK(isMnemonic("ASO"));
    CHECK(!isMnemonic("MOV"));

    // levels
    CHECK(allowedAt(0xA9, CpuLevel::Documented));
    CHECK(!allowedAt(0xA7, CpuLevel::Documented));
    CHECK(allowedAt(0xA7, CpuLevel::Stable));
    CHECK(!allowedAt(0x8B, CpuLevel::Stable));
    CHECK(allowedAt(0x8B, CpuLevel::All));
    CHECK(!allowedAt(0x02, CpuLevel::Stable));
    CHECK(allowedAt(0x02, CpuLevel::All));

    CHECK_EQ(modeLength(Mode::Implied), 1U);
    CHECK_EQ(modeLength(Mode::IndirectY), 2U);
    CHECK_EQ(modeLength(Mode::Indirect), 3U);

    return finish("opcodes");
}
