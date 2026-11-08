#include "m6510/opcodes.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace m6510 {

namespace {

// Shorthands for the table below.
constexpr Mode IMP = Mode::Implied, ACC = Mode::Accumulator, IMM = Mode::Immediate, ZP = Mode::ZeroPage,
               ZPX = Mode::ZeroPageX, ZPY = Mode::ZeroPageY, ABS = Mode::Absolute, ABX = Mode::AbsoluteX,
               ABY = Mode::AbsoluteY, IND = Mode::Indirect, IZX = Mode::IndirectX, IZY = Mode::IndirectY,
               REL = Mode::Relative;
constexpr OpKind D = OpKind::Documented, I = OpKind::Illegal, U = OpKind::Unstable, J = OpKind::Jam;
constexpr bool P = true, _ = false;

// Raw table: mnemonic, mode, cycles, page penalty, kind. `preferred` is computed afterwards.
struct Raw {
    const char* m;
    Mode mode;
    std::uint8_t cycles;
    bool page;
    OpKind kind;
};

// clang-format off
constexpr Raw kRaw[256] = {
    // 0x00
    {"BRK",IMP,7,_,D}, {"ORA",IZX,6,_,D}, {"JAM",IMP,0,_,J}, {"SLO",IZX,8,_,I},
    {"NOP",ZP ,3,_,I}, {"ORA",ZP ,3,_,D}, {"ASL",ZP ,5,_,D}, {"SLO",ZP ,5,_,I},
    {"PHP",IMP,3,_,D}, {"ORA",IMM,2,_,D}, {"ASL",ACC,2,_,D}, {"ANC",IMM,2,_,I},
    {"NOP",ABS,4,_,I}, {"ORA",ABS,4,_,D}, {"ASL",ABS,6,_,D}, {"SLO",ABS,6,_,I},
    // 0x10
    {"BPL",REL,2,P,D}, {"ORA",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"SLO",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"ORA",ZPX,4,_,D}, {"ASL",ZPX,6,_,D}, {"SLO",ZPX,6,_,I},
    {"CLC",IMP,2,_,D}, {"ORA",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"SLO",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"ORA",ABX,4,P,D}, {"ASL",ABX,7,_,D}, {"SLO",ABX,7,_,I},
    // 0x20
    {"JSR",ABS,6,_,D}, {"AND",IZX,6,_,D}, {"JAM",IMP,0,_,J}, {"RLA",IZX,8,_,I},
    {"BIT",ZP ,3,_,D}, {"AND",ZP ,3,_,D}, {"ROL",ZP ,5,_,D}, {"RLA",ZP ,5,_,I},
    {"PLP",IMP,4,_,D}, {"AND",IMM,2,_,D}, {"ROL",ACC,2,_,D}, {"ANC",IMM,2,_,I},
    {"BIT",ABS,4,_,D}, {"AND",ABS,4,_,D}, {"ROL",ABS,6,_,D}, {"RLA",ABS,6,_,I},
    // 0x30
    {"BMI",REL,2,P,D}, {"AND",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"RLA",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"AND",ZPX,4,_,D}, {"ROL",ZPX,6,_,D}, {"RLA",ZPX,6,_,I},
    {"SEC",IMP,2,_,D}, {"AND",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"RLA",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"AND",ABX,4,P,D}, {"ROL",ABX,7,_,D}, {"RLA",ABX,7,_,I},
    // 0x40
    {"RTI",IMP,6,_,D}, {"EOR",IZX,6,_,D}, {"JAM",IMP,0,_,J}, {"SRE",IZX,8,_,I},
    {"NOP",ZP ,3,_,I}, {"EOR",ZP ,3,_,D}, {"LSR",ZP ,5,_,D}, {"SRE",ZP ,5,_,I},
    {"PHA",IMP,3,_,D}, {"EOR",IMM,2,_,D}, {"LSR",ACC,2,_,D}, {"ALR",IMM,2,_,I},
    {"JMP",ABS,3,_,D}, {"EOR",ABS,4,_,D}, {"LSR",ABS,6,_,D}, {"SRE",ABS,6,_,I},
    // 0x50
    {"BVC",REL,2,P,D}, {"EOR",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"SRE",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"EOR",ZPX,4,_,D}, {"LSR",ZPX,6,_,D}, {"SRE",ZPX,6,_,I},
    {"CLI",IMP,2,_,D}, {"EOR",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"SRE",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"EOR",ABX,4,P,D}, {"LSR",ABX,7,_,D}, {"SRE",ABX,7,_,I},
    // 0x60
    {"RTS",IMP,6,_,D}, {"ADC",IZX,6,_,D}, {"JAM",IMP,0,_,J}, {"RRA",IZX,8,_,I},
    {"NOP",ZP ,3,_,I}, {"ADC",ZP ,3,_,D}, {"ROR",ZP ,5,_,D}, {"RRA",ZP ,5,_,I},
    {"PLA",IMP,4,_,D}, {"ADC",IMM,2,_,D}, {"ROR",ACC,2,_,D}, {"ARR",IMM,2,_,I},
    {"JMP",IND,5,_,D}, {"ADC",ABS,4,_,D}, {"ROR",ABS,6,_,D}, {"RRA",ABS,6,_,I},
    // 0x70
    {"BVS",REL,2,P,D}, {"ADC",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"RRA",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"ADC",ZPX,4,_,D}, {"ROR",ZPX,6,_,D}, {"RRA",ZPX,6,_,I},
    {"SEI",IMP,2,_,D}, {"ADC",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"RRA",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"ADC",ABX,4,P,D}, {"ROR",ABX,7,_,D}, {"RRA",ABX,7,_,I},
    // 0x80
    {"NOP",IMM,2,_,I}, {"STA",IZX,6,_,D}, {"NOP",IMM,2,_,I}, {"SAX",IZX,6,_,I},
    {"STY",ZP ,3,_,D}, {"STA",ZP ,3,_,D}, {"STX",ZP ,3,_,D}, {"SAX",ZP ,3,_,I},
    {"DEY",IMP,2,_,D}, {"NOP",IMM,2,_,I}, {"TXA",IMP,2,_,D}, {"ANE",IMM,2,_,U},
    {"STY",ABS,4,_,D}, {"STA",ABS,4,_,D}, {"STX",ABS,4,_,D}, {"SAX",ABS,4,_,I},
    // 0x90
    {"BCC",REL,2,P,D}, {"STA",IZY,6,_,D}, {"JAM",IMP,0,_,J}, {"SHA",IZY,6,_,U},
    {"STY",ZPX,4,_,D}, {"STA",ZPX,4,_,D}, {"STX",ZPY,4,_,D}, {"SAX",ZPY,4,_,I},
    {"TYA",IMP,2,_,D}, {"STA",ABY,5,_,D}, {"TXS",IMP,2,_,D}, {"TAS",ABY,5,_,U},
    {"SHY",ABX,5,_,U}, {"STA",ABX,5,_,D}, {"SHX",ABY,5,_,U}, {"SHA",ABY,5,_,U},
    // 0xA0
    {"LDY",IMM,2,_,D}, {"LDA",IZX,6,_,D}, {"LDX",IMM,2,_,D}, {"LAX",IZX,6,_,I},
    {"LDY",ZP ,3,_,D}, {"LDA",ZP ,3,_,D}, {"LDX",ZP ,3,_,D}, {"LAX",ZP ,3,_,I},
    {"TAY",IMP,2,_,D}, {"LDA",IMM,2,_,D}, {"TAX",IMP,2,_,D}, {"LXA",IMM,2,_,U},
    {"LDY",ABS,4,_,D}, {"LDA",ABS,4,_,D}, {"LDX",ABS,4,_,D}, {"LAX",ABS,4,_,I},
    // 0xB0
    {"BCS",REL,2,P,D}, {"LDA",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"LAX",IZY,5,P,I},
    {"LDY",ZPX,4,_,D}, {"LDA",ZPX,4,_,D}, {"LDX",ZPY,4,_,D}, {"LAX",ZPY,4,_,I},
    {"CLV",IMP,2,_,D}, {"LDA",ABY,4,P,D}, {"TSX",IMP,2,_,D}, {"LAS",ABY,4,P,U},
    {"LDY",ABX,4,P,D}, {"LDA",ABX,4,P,D}, {"LDX",ABY,4,P,D}, {"LAX",ABY,4,P,I},
    // 0xC0
    {"CPY",IMM,2,_,D}, {"CMP",IZX,6,_,D}, {"NOP",IMM,2,_,I}, {"DCP",IZX,8,_,I},
    {"CPY",ZP ,3,_,D}, {"CMP",ZP ,3,_,D}, {"DEC",ZP ,5,_,D}, {"DCP",ZP ,5,_,I},
    {"INY",IMP,2,_,D}, {"CMP",IMM,2,_,D}, {"DEX",IMP,2,_,D}, {"SBX",IMM,2,_,I},
    {"CPY",ABS,4,_,D}, {"CMP",ABS,4,_,D}, {"DEC",ABS,6,_,D}, {"DCP",ABS,6,_,I},
    // 0xD0
    {"BNE",REL,2,P,D}, {"CMP",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"DCP",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"CMP",ZPX,4,_,D}, {"DEC",ZPX,6,_,D}, {"DCP",ZPX,6,_,I},
    {"CLD",IMP,2,_,D}, {"CMP",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"DCP",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"CMP",ABX,4,P,D}, {"DEC",ABX,7,_,D}, {"DCP",ABX,7,_,I},
    // 0xE0
    {"CPX",IMM,2,_,D}, {"SBC",IZX,6,_,D}, {"NOP",IMM,2,_,I}, {"ISC",IZX,8,_,I},
    {"CPX",ZP ,3,_,D}, {"SBC",ZP ,3,_,D}, {"INC",ZP ,5,_,D}, {"ISC",ZP ,5,_,I},
    {"INX",IMP,2,_,D}, {"SBC",IMM,2,_,D}, {"NOP",IMP,2,_,D}, {"USBC",IMM,2,_,I},
    {"CPX",ABS,4,_,D}, {"SBC",ABS,4,_,D}, {"INC",ABS,6,_,D}, {"ISC",ABS,6,_,I},
    // 0xF0
    {"BEQ",REL,2,P,D}, {"SBC",IZY,5,P,D}, {"JAM",IMP,0,_,J}, {"ISC",IZY,8,_,I},
    {"NOP",ZPX,4,_,I}, {"SBC",ZPX,4,_,D}, {"INC",ZPX,6,_,D}, {"ISC",ZPX,6,_,I},
    {"SED",IMP,2,_,D}, {"SBC",ABY,4,P,D}, {"NOP",IMP,2,_,I}, {"ISC",ABY,7,_,I},
    {"NOP",ABX,4,P,I}, {"SBC",ABX,4,P,D}, {"INC",ABX,7,_,D}, {"ISC",ABX,7,_,I},
};
// clang-format on

struct Tables {
    std::array<OpInfo, 256> ops{};
    std::map<std::pair<std::string, Mode>, Byte> preferred;  // (mnemonic, mode) -> opcode
    std::map<std::string, int> mnemonics;                     // known canonical names

    Tables() {
        // Preferred opcode for a (mnemonic, mode): the most "official" kind, then the lowest opcode.
        for (int pass = 0; pass < 4; ++pass) {
            for (int op = 0; op < 256; ++op) {
                const Raw& r = kRaw[op];
                if (static_cast<int>(r.kind) != pass) continue;
                preferred.try_emplace({r.m, r.mode}, static_cast<Byte>(op));
            }
        }
        for (int op = 0; op < 256; ++op) {
            const Raw& r = kRaw[op];
            const bool pref = preferred.at({r.m, r.mode}) == op;
            ops[static_cast<std::size_t>(op)] = OpInfo{r.m, r.mode, r.cycles, r.page, r.kind, pref};
            mnemonics[r.m] = 1;
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const std::array<OpInfo, 256>& opcodeTable() { return tables().ops; }

std::string canonicalMnemonic(const std::string& name) {
    static const std::map<std::string, std::string> kSynonyms = {
        {"ASO", "SLO"}, {"LSE", "SRE"}, {"AAX", "SAX"}, {"DCM", "DCP"}, {"ISB", "ISC"}, {"INS", "ISC"},
        {"ASR", "ALR"}, {"XAA", "ANE"}, {"AHX", "SHA"}, {"AXA", "SHA"}, {"SHS", "TAS"}, {"XAS", "TAS"},
        {"SXA", "SHX"}, {"SYA", "SHY"}, {"LAR", "LAS"}, {"KIL", "JAM"}, {"HLT", "JAM"}, {"ATX", "LXA"},
    };
    std::string u = upper(name);
    if (auto it = kSynonyms.find(u); it != kSynonyms.end()) return it->second;
    return u;
}

bool isMnemonic(const std::string& name) { return tables().mnemonics.count(canonicalMnemonic(name)) != 0; }

std::optional<Byte> findOpcode(const std::string& mnemonic, Mode mode) {
    const auto& p = tables().preferred;
    if (auto it = p.find({mnemonic, mode}); it != p.end()) return it->second;
    return std::nullopt;
}

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Implied: return "implied";
        case Mode::Accumulator: return "A";
        case Mode::Immediate: return "#imm";
        case Mode::ZeroPage: return "zp";
        case Mode::ZeroPageX: return "zp,X";
        case Mode::ZeroPageY: return "zp,Y";
        case Mode::Absolute: return "abs";
        case Mode::AbsoluteX: return "abs,X";
        case Mode::AbsoluteY: return "abs,Y";
        case Mode::Indirect: return "(abs)";
        case Mode::IndirectX: return "(zp,X)";
        case Mode::IndirectY: return "(zp),Y";
        case Mode::Relative: return "rel";
    }
    return "?";
}

}  // namespace m6510
