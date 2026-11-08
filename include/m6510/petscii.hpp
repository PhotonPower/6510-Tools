#pragma once

#include <optional>
#include <string>
#include <vector>

#include "m6510/memory.hpp"

namespace m6510 {

/// ASCII -> PETSCII as used by `.PETSCII` (same convention as ACME `!pet` and 64tass): lower-case
/// letters become the unshifted letter codes $41-$5A, upper-case letters the shifted codes $C1-$DA,
/// so mixed-case text appears correctly in the upper/lower-case character set. Digits, space and
/// most punctuation are unchanged. Returns nothing for characters without a PETSCII equivalent.
[[nodiscard]] std::optional<Byte> asciiToPetscii(char c);

/// ASCII -> screen code as used by `.SCREEN`: `@` = 0, lower-case letters 1-26, upper-case letters
/// $41-$5A, space, digits and punctuation $20-$3F unchanged.
[[nodiscard]] std::optional<Byte> asciiToScreenCode(char c);

/// PETSCII byte as readable text: printable characters as ASCII (letters upper case), control codes
/// by name (`{CLR}`, `{RED}`, `{DOWN}`, ...), everything else as `{$xx}`.
[[nodiscard]] std::string petsciiToText(Byte b);

/// Screen code -> printable ASCII character for a text view of the screen (`@`, letters, digits and
/// punctuation; reversed characters like the normal ones; '.' for graphics characters).
[[nodiscard]] char screenCodeToChar(Byte code);

/// BASIC V2 keyword for a token ($80-$CB, $FF = pi); empty for other bytes.
[[nodiscard]] std::string basicToken(Byte token);

/// One line of a tokenised BASIC program.
struct BasicLine {
    Address address = 0;  ///< where the line starts (its link field)
    Address next = 0;     ///< value of the link field
    unsigned number = 0;  ///< line number
    std::string text;     ///< detokenised text
};

/// Detokenises the BASIC program starting at `start` (normally $0801). Stops at a zero link, at a
/// link that does not point forward, after `maxLines` lines or at the end of memory; `error` is
/// set for a malformed program (the lines read so far are returned).
[[nodiscard]] std::vector<BasicLine> detokenizeBasic(const Memory& memory, Address start, std::string* error = nullptr,
                                                     std::size_t maxLines = 10000);

/// The BASIC starter `line SYS target` at address `at`, exactly as `.BASIC line,target` assembles it:
/// link, line number, SYS token, decimal digits, 0, and the end marker 0,0.
[[nodiscard]] std::vector<Byte> makeBasicStub(Address at, unsigned line, unsigned target);

struct SysStub {
    unsigned line = 0;
    Address target = 0;
    std::size_t length = 0;  ///< bytes of the starter including the end marker
};

/// Recognises a BASIC starter at `at` that is byte-for-byte what `makeBasicStub` would produce.
[[nodiscard]] std::optional<SysStub> parseSysStub(const Memory& memory, Address at);

/// Finds the target of a `SYS` in the first line of a BASIC program (also when the line contains
/// more than the SYS, e.g. `10 SYS 2064:REM`, or uses spaces): for choosing an entry point.
[[nodiscard]] std::optional<Address> findSysTarget(const Memory& memory, Address start);

}  // namespace m6510
