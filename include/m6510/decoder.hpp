#pragma once

#include <functional>
#include <string>

#include "m6510/format.hpp"
#include "m6510/instruction.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

struct DecodeOptions {
    NumberStyle style = NumberStyle::Dollar;
    /// Which opcodes count as instructions; the others become `.BYTE $xx` (valid = false).
    CpuLevel level = CpuLevel::Stable;
    /// Optional: returns a symbol name for an address ("" = none). Used for branch and jump
    /// targets and for memory operands (not for immediates).
    std::function<std::string(Address)> symbolizer;
};

/// Decodes the instruction at `address` (stateless, never fails).
///
/// Operands are written in the syntax of docs/assembler-syntax.md so that the text assembles to
/// the same bytes: an absolute operand whose value fits into the zero page gets the `.W` suffix
/// (`LDA.W $0012`) when a zero-page form exists, and the accumulator mode is written `ASL A`.
[[nodiscard]] Instruction decode(const Memory& memory, Address address, const DecodeOptions& options = {});

/// True if a zero-page counterpart of an absolute mode exists for `mnemonic` (canonical, upper case):
/// abs -> zp, abs,X -> zp,X, abs,Y -> zp,Y.
[[nodiscard]] bool hasZeroPageForm(const std::string& mnemonic, Mode absoluteMode);

}  // namespace m6510
