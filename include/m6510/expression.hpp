#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace m6510 {

struct ExprResult {
    bool ok = false;            ///< false: syntax error or division by zero (see `error`)
    std::int64_t value = 0;
    bool undefined = false;     ///< an undefined symbol was used (value is then not final)
    std::string undefinedName;  ///< the first undefined symbol
    std::string error;
};

/// Looks up a symbol; returns false if it is not defined (yet).
using SymbolLookup = std::function<bool(const std::string& name, std::int64_t& value)>;

/// Integer expression evaluator (MOS / Commodore syntax, see docs/assembler-syntax.md).
///
/// Numbers: `123`, `$1F`, `0x1F`, `1Fh`, `%1010`, `0b1010`, `'A'`.
/// Operators (C precedence): `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`; unary `- + ~`; `( )` and `[ ]`.
/// A leading `<` or `>` takes the low / high byte of the whole following expression (weaker than
/// every other operator); `LOW(x)` and `HIGH(x)` do the same as functions. `*` where a value is
/// expected is the current address (`pc`). Values are 64 bit.
///
/// With `hexDefault` (monitor mode) a bare number such as `C000`, `FF` or `1B` is hexadecimal, `100.` is
/// decimal, and a name that is not a known symbol but consists of hex digits (`D020`) is a hex number;
/// known symbols always win.
class ExpressionEvaluator {
public:
    ExpressionEvaluator(SymbolLookup lookup, std::int64_t pc, bool hexDefault = false)
        : lookup_(std::move(lookup)), pc_(pc), hexDefault_(hexDefault) {}
    [[nodiscard]] ExprResult evaluate(const std::string& text) const;

private:
    SymbolLookup lookup_;
    std::int64_t pc_;
    bool hexDefault_;
};

/// True for characters that may start a symbol name (letters, `_`, `.` for local labels).
[[nodiscard]] bool isSymbolStart(char c);
/// True for characters that may continue a symbol name.
[[nodiscard]] bool isSymbolChar(char c);

}  // namespace m6510
