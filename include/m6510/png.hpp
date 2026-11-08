#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "m6510/types.hpp"

namespace m6510 {

/// An image with a colour palette (at most 256 entries); pixels are palette indices.
struct IndexedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> palette;  ///< 0xRRGGBB
    std::vector<Byte> pixels;            ///< width * height palette indices, row by row

    bool operator==(const IndexedImage&) const = default;
};

/// Encodes an 8-bit palette PNG (uncompressed deflate blocks; no external library needed).
std::vector<Byte> encodePng(const IndexedImage& image);

/// Standard Base64 (RFC 4648) with padding.
std::string base64Encode(const std::vector<Byte>& data);

}  // namespace m6510
