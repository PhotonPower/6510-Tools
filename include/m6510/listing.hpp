#pragma once

#include <cstdint>
#include <string>

#include "m6510/analysis.hpp"
#include "m6510/annotations.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

struct ListingOptions {
    Address from = 0;
    std::uint32_t to = 0;      ///< range [from, to)
    bool addresses = false;    ///< append each line's address as a comment (to refer to it in annotations)
    bool verify = true;        ///< assemble the listing again and compare the bytes
    std::size_t maxChars = 0;  ///< 0 = unlimited; otherwise cut at a line boundary (the cut text is not verified)
    bool c64Names = true;      ///< KERNAL entries and chip registers by name (`CHROUT`, `EXTCOL`)
    /// Title, `.CPU`, `NAME = $XXXX` lines and `* =`: needed to assemble the listing again; an excerpt for a reader can do
    /// without them (they are left out only if `verify` is off).
    bool equates = true;
};

struct ListingResult {
    std::string text;
    bool verified = false;      ///< the listing assembles to exactly the bytes of the range
    bool truncated = false;
    std::string problem;        ///< why verification failed
    unsigned unnamed = 0;       ///< generated placeholder labels (sub_/loc_/data_) that still await a real name
    unsigned instructions = 0;  ///< instruction lines in the listing
    unsigned userComments = 0;  ///< instruction lines that carry a comment from the annotations
};

/// Writes an assembler listing of a range (docs/reverse-engineering.md): instructions from the analysis
/// with names and comments from the annotations, data as `.BYTE`/`.WORD`/`.LO`/`.HI`/`.SCREEN`/`.FILL`,
/// a BASIC starter as `.BASIC`, routine headers, labels (`name+n` in the middle of an instruction), and
/// `NAME = $XXXX` lines for KERNAL entries, chip registers and variables outside the range. It is
/// generated from the real bytes, never from the annotations, and (with `verify`) assembled again and
/// compared byte for byte, so a wrong name or comment cannot change what the program does.
[[nodiscard]] ListingResult renderListing(const Memory& view, const Analysis& analysis, const Annotations& annotations,
                                          const ListingOptions& options);

}  // namespace m6510
