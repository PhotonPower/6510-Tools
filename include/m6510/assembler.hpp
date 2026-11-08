#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "m6510/opcodes.hpp"
#include "m6510/types.hpp"

namespace m6510 {

struct AssemblerOptions {
    Address origin = 0x0801;                         ///< start address before the first `* =` / ORG
    std::map<std::string, std::int64_t> predefined;  ///< symbols defined before assembly (-D)
    CpuLevel level = CpuLevel::Stable;               ///< allowed opcodes until a `.CPU` directive
};

struct Diagnostic {
    int line = 0;  ///< 1-based source line (0 = not tied to a line)
    std::string message;
};

/// A contiguous block of output bytes.
struct Segment {
    Address address = 0;
    std::vector<Byte> bytes;
};

struct ListingLine {
    int line = 0;
    Address address = 0;
    std::vector<Byte> bytes;
    std::string source;
};

struct AssemblyResult {
    std::vector<Segment> segments;
    std::vector<Diagnostic> errors;
    std::map<std::string, std::int64_t> symbols;  ///< labels and constants (local labels as `global.local`)
    std::vector<ListingLine> listing;

    [[nodiscard]] bool ok() const { return errors.empty(); }
    [[nodiscard]] bool empty() const;

    /// Lowest / one past the highest address of any output byte (0 if there is none).
    [[nodiscard]] Address lowest() const;
    [[nodiscard]] std::uint32_t end() const;
    /// All segments as one raw image starting at lowest(); gaps are filled with zero bytes.
    [[nodiscard]] std::vector<Byte> flatten() const;
    /// The listing as text: line number, address, bytes, source.
    [[nodiscard]] std::string formatListing() const;
    /// The symbol table as text ("NAME = $XXXX"), sorted by name.
    [[nodiscard]] std::string formatSymbols() const;
    /// VICE monitor label file ("al C:080d .start"), labels with values $0000-$FFFF.
    [[nodiscard]] std::string formatViceLabels() const;
};

/// Assembles 6510 source text; see docs/assembler-syntax.md.
///
/// The assembler runs several passes until symbol values and the forms chosen for zero page /
/// absolute operands stop changing; forms only ever grow, so this converges. Errors do not stop the
/// run; all errors of the final pass are collected (the bytes of faulty lines are omitted).
[[nodiscard]] AssemblyResult assemble(const std::string& source, const AssemblerOptions& options = {});

/// True if `name` is a valid symbol name that is not reserved (registers, mnemonics).
[[nodiscard]] bool isValidSymbolName(const std::string& name, std::string* why = nullptr);

}  // namespace m6510
