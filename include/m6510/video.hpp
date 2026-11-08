#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "m6510/c64.hpp"
#include "m6510/memory.hpp"
#include "m6510/png.hpp"

namespace m6510 {

/// The 16 colours of the VIC-II as 0xRRGGBB.
enum class Palette {
    Colodore,  ///< the newer measurement from colodore.com (default)
    Pepto      ///< the classic measurement from pepto.de
};
[[nodiscard]] const std::array<std::uint32_t, 16>& paletteColors(Palette palette);
/// "colodore" / "pepto" -> Palette; false for an unknown name.
bool parsePalette(const std::string& name, Palette& out);

struct RenderOptions {
    bool border = false;               ///< add the border (32 px left/right, 36 px top/bottom: 384 x 272)
    Palette palette = Palette::Colodore;
};

/// The C64 screen rendered from the machine state (docs/c64.md, "Video"): the VIC-II registers and memory
/// as they are at the moment, not line by line. Text (standard, multicolour, extended background) and
/// bitmap modes (hires, multicolour), display enable, 38 columns / 24 rows, fine scrolling and the
/// VIC bank. Without a character ROM a self-drawn replacement character set is used. No sprites.
/// The result is 320 x 200 pixels (384 x 272 with border), palette indices 0..15.
IndexedImage renderScreen(const C64& c64, const Memory& ram, const RenderOptions& options = {});

/// Cuts out the pixel box [x0,x1] x [y0,y1] (clamped to the image) and enlarges it by an integer
/// factor (nearest neighbour).
IndexedImage cropAndZoom(const IndexedImage& image, int x0, int y0, int x1, int y1, int zoom);

struct ImageDiff {
    bool sameSize = true;
    std::uint64_t changedPixels = 0;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  ///< bounding box of the change (inclusive); empty if x1 < x0
};
ImageDiff diffImages(const IndexedImage& a, const IndexedImage& b);

/// The replacement character set (upper case / graphics, or lower / upper case): 128 glyphs of 8 bytes.
[[nodiscard]] const std::array<Byte, 1024>& replacementCharset(bool lowerCase);

}  // namespace m6510
