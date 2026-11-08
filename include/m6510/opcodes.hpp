#pragma once

#include <array>
#include <optional>
#include <string>

#include "m6510/types.hpp"

namespace m6510 {

/// The 13 addressing modes of the 6502 family.
enum class Mode : std::uint8_t {
    Implied,      ///< INX
    Accumulator,  ///< ASL A
    Immediate,    ///< LDA #$01
    ZeroPage,     ///< LDA $FB
    ZeroPageX,    ///< LDA $10,X
    ZeroPageY,    ///< LDX $10,Y
    Absolute,     ///< LDA $C000
    AbsoluteX,    ///< LDA $0400,X
    AbsoluteY,    ///< LDA $0400,Y
    Indirect,     ///< JMP ($0314)
    IndirectX,    ///< LDA ($20,X)
    IndirectY,    ///< LDA ($FB),Y
    Relative      ///< BNE label
};

/// How "official" an opcode is. The numeric order is the CPU level that allows it (see `CpuLevel`).
enum class OpKind : std::uint8_t {
    Documented,  ///< one of the 151 documented opcodes
    Illegal,     ///< undocumented, but stable on all NMOS 6510s (LAX, SAX, DCP, ...)
    Unstable,    ///< undocumented, result depends on the chip (ANE, LXA, SHA, SHX, SHY, TAS, LAS)
    Jam          ///< halts the processor (JAM/KIL)
};

/// Which opcodes the assembler accepts and the decoder shows as instructions.
enum class CpuLevel : std::uint8_t {
    Documented = 0,  ///< `.CPU 6502`, `--no-illegal`
    Stable = 1,      ///< `.CPU 6510` (default): documented + stable undocumented
    All = 2          ///< `.CPU 6510X`, `--unstable`: everything, including unstable opcodes and JAM
};

/// One entry of the opcode table.
struct OpInfo {
    const char* mnemonic;  ///< upper case; undocumented opcodes use the names of "No More Secrets"
    Mode mode;
    std::uint8_t cycles;   ///< base cycles (taken branches and page crossings come on top)
    bool pagePenalty;      ///< +1 cycle when an indexed read crosses a page (and branches: see docs)
    OpKind kind;
    /// False for a duplicate of another opcode with the same mnemonic and mode (e.g. the undocumented
    /// NOP variants, ANC $2B, JAM except $02). The assembler always produces the preferred opcode;
    /// the disassembler writes the others as `.BYTE` in source mode.
    bool preferred;
};

/// The opcode table (256 entries, indexed by opcode).
[[nodiscard]] const std::array<OpInfo, 256>& opcodeTable();
[[nodiscard]] inline const OpInfo& opInfo(Byte opcode) { return opcodeTable()[opcode]; }

/// Instruction length in bytes for an addressing mode (1-3).
[[nodiscard]] constexpr unsigned modeLength(Mode mode) {
    switch (mode) {
        case Mode::Implied:
        case Mode::Accumulator:
            return 1;
        case Mode::Absolute:
        case Mode::AbsoluteX:
        case Mode::AbsoluteY:
        case Mode::Indirect:
            return 3;
        default:
            return 2;
    }
}

/// The minimum CPU level at which an opcode kind is allowed.
[[nodiscard]] constexpr CpuLevel levelOf(OpKind kind) {
    switch (kind) {
        case OpKind::Documented:
            return CpuLevel::Documented;
        case OpKind::Illegal:
            return CpuLevel::Stable;
        default:
            return CpuLevel::All;
    }
}

/// True if `opcode` is an instruction at `level`.
[[nodiscard]] inline bool allowedAt(Byte opcode, CpuLevel level) {
    return static_cast<int>(levelOf(opInfo(opcode).kind)) <= static_cast<int>(level);
}

/// Canonical mnemonic for a name or one of its common synonyms (DCM -> DCP, ISB -> ISC, ...).
/// Returns the upper-case name unchanged if it is no synonym.
[[nodiscard]] std::string canonicalMnemonic(const std::string& name);

/// True if `name` (any case, synonyms included) is a mnemonic of any opcode.
[[nodiscard]] bool isMnemonic(const std::string& name);

/// The preferred opcode for a canonical mnemonic and mode, if one exists (regardless of level).
[[nodiscard]] std::optional<Byte> findOpcode(const std::string& mnemonic, Mode mode);

/// Short name of a mode for messages and tables ("abs,X", "(zp),Y", ...).
[[nodiscard]] const char* modeName(Mode mode);

}  // namespace m6510
