#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "m6510/image_io.hpp"
#include "m6510/types.hpp"

namespace m6510 {

/// One entry of a 1541 directory.
struct D64Entry {
    std::string name;          ///< as text (PETSCII converted like petsciiToText, the $A0 padding removed)
    std::vector<Byte> rawName; ///< the 16 bytes as stored (PETSCII, padded with $A0)
    std::string type;          ///< "DEL", "SEQ", "PRG", "USR", "REL" or "???"
    bool closed = true;        ///< bit 7 of the type byte (an unclosed file is shown as "*PRG")
    bool locked = false;       ///< bit 6 ("PRG<")
    unsigned track = 0, sector = 0;  ///< first sector of the file
    unsigned blocks = 0;       ///< size in blocks as the directory says
    unsigned index = 0;        ///< position in the directory (0-based)
};

/// Read access to a 1541 disk image (D64, docs/formats.md): 35 or 40 tracks, with or without the error
/// bytes. Only reading. Damaged images are read as far as possible; what was wrong is collected in
/// `problems()` (loops in chains, links outside the disk, wrong sizes).
class D64Image {
public:
    /// Throws IoError if the size is not one of 174848, 175531, 196608 or 197376 bytes.
    explicit D64Image(std::vector<Byte> bytes);

    [[nodiscard]] unsigned tracks() const { return tracks_; }
    [[nodiscard]] bool hasErrorBytes() const { return hasErrors_; }
    [[nodiscard]] static unsigned sectorsPerTrack(unsigned track);
    /// The 256 bytes of a sector. Throws IoError for a track/sector that is not on the disk.
    [[nodiscard]] std::vector<Byte> block(unsigned track, unsigned sector) const;
    [[nodiscard]] bool validBlock(unsigned track, unsigned sector) const;
    /// The error code of a sector (1 = no error) if the image has error bytes.
    [[nodiscard]] std::optional<Byte> errorCode(unsigned track, unsigned sector) const;
    /// Sectors whose error code says something other than "no error".
    [[nodiscard]] unsigned badSectors() const;

    [[nodiscard]] std::string diskName() const;  ///< from the BAM (as text)
    [[nodiscard]] std::string diskId() const;    ///< two ID characters
    [[nodiscard]] std::string dosType() const;   ///< usually "2A"
    [[nodiscard]] unsigned freeBlocks() const;   ///< from the BAM, without track 18

    /// The directory (from track 18, sector 1). Empty entries (type byte 0) are skipped.
    [[nodiscard]] std::vector<D64Entry> directory() const;
    /// Finds a file by name. Patterns as on the C64: `*` matches the rest, `?` one character; "*" alone =
    /// the first file. The comparison is made on the text names, ignoring case.
    [[nodiscard]] std::optional<D64Entry> find(const std::string& pattern) const;
    /// The contents of a file (for a PRG: with its two-byte load address). Throws IoError for a chain that
    /// leaves the disk or loops.
    [[nodiscard]] std::vector<Byte> readFile(const D64Entry& entry) const;
    /// A PRG file as a Program (load address + bytes).
    [[nodiscard]] Program readProgram(const D64Entry& entry) const;

    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

private:
    [[nodiscard]] std::size_t offset(unsigned track, unsigned sector) const;
    std::vector<Byte> data_;
    unsigned tracks_ = 35;
    bool hasErrors_ = false;
    mutable std::vector<std::string> problems_;
};

/// True if a pattern (`*`, `?`) matches a name the way the 1541 compares them (case-insensitive here).
[[nodiscard]] bool cbmNameMatches(const std::string& pattern, const std::string& name);

}  // namespace m6510
