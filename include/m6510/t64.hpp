#pragma once

#include <optional>
#include <string>
#include <vector>

#include "m6510/image_io.hpp"
#include "m6510/types.hpp"

namespace m6510 {

/// One file of a T64 tape archive.
struct T64Entry {
    std::string name;            ///< as text (PETSCII converted, trailing padding removed)
    Byte entryType = 1;          ///< 1 = normal file, 3 = memory snapshot, 0 = free
    Byte fileType = 0x82;        ///< 1541 file type byte ($82 = PRG)
    Address start = 0, end = 0;  ///< load address and end address (exclusive) as stored
    std::uint32_t offset = 0;    ///< position of the data in the archive
    std::uint32_t length = 0;    ///< bytes really there (corrected, see T64Image)
    unsigned index = 0;
};

/// Read access to a T64 tape archive (docs/formats.md). Known faults of many T64 files are tolerated and
/// reported in `problems()`: an end address that does not fit the data (the length is then taken from the
/// offsets / the file size) and a used-entries count of 0.
class T64Image {
public:
    /// Throws IoError for a file that is not a T64 archive (signature, sizes).
    explicit T64Image(std::vector<Byte> bytes);

    [[nodiscard]] const std::string& tapeName() const { return name_; }
    [[nodiscard]] unsigned version() const { return version_; }
    [[nodiscard]] const std::vector<T64Entry>& entries() const { return entries_; }
    [[nodiscard]] std::optional<T64Entry> find(const std::string& pattern) const;  ///< patterns as for D64 (`*`, `?`)
    [[nodiscard]] Program readProgram(const T64Entry& entry) const;
    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

private:
    std::vector<Byte> data_;
    std::string name_;
    unsigned version_ = 0;
    std::vector<T64Entry> entries_;
    std::vector<std::string> problems_;
};

}  // namespace m6510
