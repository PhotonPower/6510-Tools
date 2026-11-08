#include "m6510/crt.hpp"

#include <cstring>

#include "m6510/image_io.hpp"

namespace m6510 {

namespace {

unsigned be16(const std::vector<Byte>& d, std::size_t o) { return static_cast<unsigned>((d[o] << 8) | d[o + 1]); }
std::uint32_t be32(const std::vector<Byte>& d, std::size_t o) {
    return (static_cast<std::uint32_t>(d[o]) << 24) | (static_cast<std::uint32_t>(d[o + 1]) << 16) | (static_cast<std::uint32_t>(d[o + 2]) << 8) |
           static_cast<std::uint32_t>(d[o + 3]);
}

}  // namespace

CrtImage::CrtImage(const std::vector<Byte>& d) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "C64 CARTRIDGE   ", 16) != 0) throw IoError("not a CRT image (no \"C64 CARTRIDGE\" signature)");
    const std::uint32_t headerLength = be32(d, 0x10);
    if (headerLength < 0x40 || headerLength > d.size()) throw IoError("CRT: bad header length " + std::to_string(headerLength));
    version_ = be16(d, 0x14);
    type_ = be16(d, 0x16);
    exrom_ = d[0x18] != 0;
    game_ = d[0x19] != 0;
    for (std::size_t i = 0x20; i < 0x40 && d[i] != 0; ++i) name_ += static_cast<char>(d[i] >= 0x20 && d[i] < 0x7F ? d[i] : '?');
    std::size_t o = headerLength;
    while (o < d.size()) {
        if (d.size() - o < 16 || std::memcmp(d.data() + o, "CHIP", 4) != 0)
            throw IoError("CRT: no CHIP packet at offset " + std::to_string(o));
        const std::uint32_t packet = be32(d, o + 4);
        CrtChip c;
        c.type = be16(d, o + 8);
        c.bank = be16(d, o + 10);
        c.load = static_cast<Address>(be16(d, o + 12));
        const unsigned size = be16(d, o + 14);
        if (packet < 16 + size || o + packet > d.size())
            throw IoError("CRT: the CHIP packet at offset " + std::to_string(o) + " is longer than the file or shorter than its data");
        if (c.load + size > 0x10000) throw IoError("CRT: a CHIP packet does not fit below $FFFF");
        c.data.assign(d.begin() + static_cast<std::ptrdiff_t>(o + 16), d.begin() + static_cast<std::ptrdiff_t>(o + 16 + size));
        chips_.push_back(std::move(c));
        o += packet;
    }
    if (chips_.empty()) throw IoError("CRT: no CHIP packets");
}

std::string CrtImage::mode() const {
    if (!exrom_ && game_) return "8K";
    if (!exrom_ && !game_) return "16K";
    if (exrom_ && !game_) return "Ultimax";
    return "off";
}

std::string CrtImage::hardwareName() const {
    static const char* names[] = {"normal cartridge", "Action Replay", "KCS Power Cartridge", "Final Cartridge III", "Simons' BASIC",
                                  "Ocean type 1", "Expert Cartridge", "Fun Play / Power Play", "Super Games", "Atomic Power",
                                  "Epyx Fastload", "Westermann Learning", "Rex Utility", "Final Cartridge I", "Magic Formel",
                                  "C64 Game System / System 3", "Warp Speed", "Dinamic", "Zaxxon / Super Zaxxon", "Magic Desk",
                                  "Super Snapshot V5", "Comal-80"};
    if (type_ < sizeof(names) / sizeof(*names)) return names[type_];
    if (type_ == 32) return "EasyFlash";
    return "type " + std::to_string(type_);
}

}  // namespace m6510
