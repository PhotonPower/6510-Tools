#pragma once

#include <string>
#include <vector>

#include "m6510/types.hpp"

namespace m6510 {

/// One CHIP packet of a cartridge image.
struct CrtChip {
    unsigned type = 0;   ///< 0 = ROM, 1 = RAM, 2 = flash ROM
    unsigned bank = 0;
    Address load = 0;    ///< $8000 (ROML) or $A000/$E000 (ROMH)
    std::vector<Byte> data;
};

/// Read access to a CRT cartridge image (docs/formats.md): header and CHIP packets. Only reading;
/// `hardwareType()` 0 (a normal 8K, 16K or Ultimax cartridge) can be inserted into the C64 machine,
/// bank-switching types can still be listed and their banks extracted.
class CrtImage {
public:
    /// Throws IoError for a file that is not a CRT image or a damaged packet.
    explicit CrtImage(const std::vector<Byte>& bytes);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] unsigned hardwareType() const { return type_; }
    [[nodiscard]] std::string hardwareName() const;   ///< "normal cartridge", "Ocean type 1", ...
    [[nodiscard]] unsigned version() const { return version_; }
    [[nodiscard]] bool exrom() const { return exrom_; }  ///< line levels as stored (0 = active)
    [[nodiscard]] bool game() const { return game_; }
    /// "8K", "16K", "Ultimax" or "off" from the EXROM/GAME lines.
    [[nodiscard]] std::string mode() const;
    [[nodiscard]] const std::vector<CrtChip>& chips() const { return chips_; }

private:
    std::string name_;
    unsigned type_ = 0, version_ = 0;
    bool exrom_ = true, game_ = true;
    std::vector<CrtChip> chips_;
};

}  // namespace m6510
