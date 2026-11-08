#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "m6510/opcodes.hpp"
#include "m6510/types.hpp"

namespace m6510 {

/// Result of evaluating an expression for the encoder.
struct EncodeValue {
    std::int64_t value = 0;
    bool undefined = false;  ///< forward reference that is not resolved yet (value is a placeholder)
    std::string error;       ///< non-empty: evaluation failed
};
using ValueFn = std::function<EncodeValue(const std::string& expression)>;

struct EncodeRequest {
    std::string mnemonic;  ///< any case, synonyms allowed: "lda", "DCM"
    char force = 0;        ///< 0, 'B' (force zero page) or 'W' (force absolute)
    std::string operand;   ///< operand text, e.g. "($FB),Y", "#<table", "A", ""
    Address pc = 0;        ///< address of the instruction (for branches)
    CpuLevel level = CpuLevel::Stable;
    /// Sizing level remembered from earlier passes: 0 = zero page if the value fits, 1 = absolute.
    /// Forms only ever grow, which makes the multi-pass assembler converge.
    int hint = 0;
};

struct EncodeResult {
    bool ok = false;
    std::string error;
    std::vector<Byte> bytes;
    int level = 0;  ///< sizing level used; feed it back as `hint` in the next pass
};

/// Encodes one instruction (syntax: docs/assembler-syntax.md). A forward reference (undefined
/// value) starts with the zero-page form where one exists and grows in a later pass.
[[nodiscard]] EncodeResult encodeInstruction(const EncodeRequest& request, const ValueFn& value);

/// Splits an operand list at top-level commas (not inside parentheses, brackets or quotes).
[[nodiscard]] std::vector<std::string> splitOperands(const std::string& text);

}  // namespace m6510
