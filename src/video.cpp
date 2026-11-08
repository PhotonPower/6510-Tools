#include "m6510/video.hpp"

#include <algorithm>

namespace m6510 {

namespace {

#include "video_font.inc"

constexpr int kWidth = 320, kHeight = 200;
constexpr int kBorderX = 32, kBorderY = 36;

}  // namespace

const std::array<std::uint32_t, 16>& paletteColors(Palette palette) {
    static const std::array<std::uint32_t, 16> colodore = {0x000000, 0xFFFFFF, 0x813338, 0x75CEC8, 0x8E3C97, 0x56AC4D,
                                                           0x2E2C9B, 0xEDF171, 0x8E5029, 0x553800, 0xC46C71, 0x4A4A4A,
                                                           0x7B7B7B, 0xA9FF9F, 0x706DEB, 0xB2B2B2};
    static const std::array<std::uint32_t, 16> pepto = {0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43,
                                                        0x352879, 0xB8C76F, 0x6F4F25, 0x433900, 0x9A6759, 0x444444,
                                                        0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595};
    return palette == Palette::Pepto ? pepto : colodore;
}

bool parsePalette(const std::string& name, Palette& out) {
    if (name == "colodore") out = Palette::Colodore;
    else if (name == "pepto") out = Palette::Pepto;
    else return false;
    return true;
}

const std::array<Byte, 1024>& replacementCharset(bool lowerCase) { return lowerCase ? kLower : kUpper; }

IndexedImage renderScreen(const C64& c64, const Memory& ram, const RenderOptions& options) {
    const Byte d011 = c64.vic(0x11), d016 = c64.vic(0x16), d018 = c64.vic(0x18);
    const Address bank = c64.vicBank();
    const bool charRomInBank = bank == 0x0000 || bank == 0x8000;
    // What the VIC reads at a 14-bit address of its bank: the character ROM at $1000-$1FFF in banks 0 and 2.
    auto fetch = [&](unsigned rel) -> Byte {
        rel &= 0x3FFF;
        if (charRomInBank && (rel & 0x3000) == 0x1000) {
            const unsigned off = rel & 0x0FFF;
            if (c64.hasChar()) return c64.charRom()[off];
            const unsigned code = (off >> 3) & 0xFF;
            const Byte b = replacementCharset((off & 0x800) != 0)[(code & 0x7F) * 8 + (off & 7)];
            return (code & 0x80) ? static_cast<Byte>(~b) : b;
        }
        return ram.read8(static_cast<Address>(bank + rel));
    };
    const unsigned screen = ((d018 >> 4) & 0x0F) * 0x400u;
    const unsigned charBase = ((d018 >> 1) & 0x07) * 0x800u;
    const unsigned bitmapBase = (d018 & 0x08) ? 0x2000u : 0u;
    const bool ecm = (d011 & 0x40) != 0, bmm = (d011 & 0x20) != 0, mcm = (d016 & 0x10) != 0;
    const bool den = (d011 & 0x10) != 0;
    const bool rsel = (d011 & 0x08) != 0, csel = (d016 & 0x08) != 0;
    const int xscroll = d016 & 7, yscroll = d011 & 7;
    const Byte border = c64.vic(0x20) & 0x0F;
    const Byte bg[4] = {static_cast<Byte>(c64.vic(0x21) & 0x0F), static_cast<Byte>(c64.vic(0x22) & 0x0F),
                        static_cast<Byte>(c64.vic(0x23) & 0x0F), static_cast<Byte>(c64.vic(0x24) & 0x0F)};

    auto pixel = [&](int x, int y) -> Byte {  // x, y inside the 320 x 200 display window
        if (!den) return border;
        if ((!csel && (x < 7 || x >= 311)) || (!rsel && (y < 4 || y >= 196))) return border;
        const int sx = x - xscroll, sy = y - (yscroll - 3);
        if (sx < 0 || sy < 0 || sx >= kWidth || sy >= kHeight) return bg[0];
        const unsigned cell = static_cast<unsigned>(sy / 8 * 40 + sx / 8);
        const unsigned line = static_cast<unsigned>(sy % 8);
        const int bit = 7 - sx % 8;
        const Byte code = fetch(screen + cell);
        const Byte color = c64.colorRam(cell) & 0x0F;
        if (ecm && (bmm || mcm)) return 0;  // invalid modes show black
        if (!bmm) {
            if (ecm) {
                const Byte b = fetch(charBase + (code & 0x3Fu) * 8 + line);
                return ((b >> bit) & 1) ? color : bg[code >> 6];
            }
            const Byte b = fetch(charBase + code * 8u + line);
            if (!mcm || !(color & 0x08)) return ((b >> bit) & 1) ? (mcm ? static_cast<Byte>(color & 7) : color) : bg[0];
            const unsigned pair = (b >> (bit & ~1)) & 3;
            return pair == 3 ? static_cast<Byte>(color & 7) : bg[pair];
        }
        const Byte b = fetch(bitmapBase + cell * 8 + line);
        if (!mcm) return ((b >> bit) & 1) ? static_cast<Byte>(code >> 4) : static_cast<Byte>(code & 0x0F);
        switch ((b >> (bit & ~1)) & 3) {
            case 0: return bg[0];
            case 1: return static_cast<Byte>(code >> 4);
            case 2: return static_cast<Byte>(code & 0x0F);
            default: return color;
        }
    };

    IndexedImage img;
    const int bx = options.border ? kBorderX : 0, by = options.border ? kBorderY : 0;
    img.width = kWidth + 2 * bx;
    img.height = kHeight + 2 * by;
    const auto& pal = paletteColors(options.palette);
    img.palette.assign(pal.begin(), pal.end());
    img.pixels.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), border);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            img.pixels[static_cast<std::size_t>(y + by) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x + bx)] = pixel(x, y);
    return img;
}

IndexedImage cropAndZoom(const IndexedImage& image, int x0, int y0, int x1, int y1, int zoom) {
    x0 = std::clamp(x0, 0, image.width - 1);
    x1 = std::clamp(x1, x0, image.width - 1);
    y0 = std::clamp(y0, 0, image.height - 1);
    y1 = std::clamp(y1, y0, image.height - 1);
    zoom = std::clamp(zoom, 1, 16);
    IndexedImage out;
    out.palette = image.palette;
    out.width = (x1 - x0 + 1) * zoom;
    out.height = (y1 - y0 + 1) * zoom;
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(x)] =
                image.pixels[static_cast<std::size_t>(y0 + y / zoom) * static_cast<std::size_t>(image.width) +
                             static_cast<std::size_t>(x0 + x / zoom)];
    return out;
}

ImageDiff diffImages(const IndexedImage& a, const IndexedImage& b) {
    ImageDiff d;
    if (a.width != b.width || a.height != b.height) {
        d.sameSize = false;
        return d;
    }
    d.x0 = a.width;
    d.y0 = a.height;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x);
            if (a.palette[a.pixels[i]] == b.palette[b.pixels[i]]) continue;
            ++d.changedPixels;
            d.x0 = std::min(d.x0, x);
            d.y0 = std::min(d.y0, y);
            d.x1 = std::max(d.x1, x);
            d.y1 = std::max(d.y1, y);
        }
    }
    if (d.changedPixels == 0) d.x0 = d.y0 = 0;
    return d;
}

}  // namespace m6510
