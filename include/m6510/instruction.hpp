#pragma once

#include <optional>
#include <string>
#include <vector>

#include "m6510/opcodes.hpp"
#include "m6510/types.hpp"

namespace m6510 {

/// How an instruction affects the flow of control.
enum class FlowKind {
    Normal,    ///< continues with the next instruction
    Jump,      ///< unconditional: JMP abs, JMP (ind)
    CondJump,  ///< conditional branch (may fall through)
    Call,      ///< JSR (returns to the next instruction)
    Return,    ///< RTS, RTI, BRK: the path ends
    Halt       ///< JAM: the processor stops
};

/// One decoded instruction (or one byte that is not shown as an instruction: `.BYTE $xx`).
struct Instruction {
    Address address = 0;
    std::vector<Byte> bytes;  ///< opcode and operand bytes
    bool valid = false;       ///< false: not an instruction at the chosen CPU level (shown as `.BYTE`)
    /// False for a non-preferred duplicate opcode (the assembler would produce another opcode for the
    /// same text); source output writes such instructions as `.BYTE`.
    bool canonical = true;
    Byte opcode = 0;
    Mode mode = Mode::Implied;
    std::string mnemonic;  ///< e.g. "LDA", "LDA.W" (forced absolute), ".BYTE"
    std::string operands;  ///< e.g. "($FB),Y"; empty if none
    FlowKind flow = FlowKind::Normal;
    /// Control-flow target: branches, JMP abs, JSR.
    std::optional<Address> target;
    /// Address of a memory operand (zero page, absolute, indexed base, pointer of an indirect mode).
    std::optional<Address> reference;
    /// The raw operand value (immediate, zero-page address, absolute address, branch target).
    unsigned value = 0;

    [[nodiscard]] std::size_t length() const { return bytes.size(); }
    [[nodiscard]] std::string text() const { return operands.empty() ? mnemonic : mnemonic + " " + operands; }
};

}  // namespace m6510
