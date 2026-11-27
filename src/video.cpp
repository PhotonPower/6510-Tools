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

namespace {

// The VIC-II state at one moment: what the graphics and the sprites show at a pixel of the 320 x 200 window.
class Frame {
public:
    Frame(const C64& c64, const Memory& ram) : c64_(c64), ram_(ram) {
        const Byte d011 = c64.vic(0x11), d016 = c64.vic(0x16), d018 = c64.vic(0x18);
        bank_ = c64.vicBank();
        charRomInBank_ = bank_ == 0x0000 || bank_ == 0x8000;
        screen_ = ((d018 >> 4) & 0x0F) * 0x400u;
        charBase_ = ((d018 >> 1) & 0x07) * 0x800u;
        bitmapBase_ = (d018 & 0x08) ? 0x2000u : 0u;
        ecm_ = (d011 & 0x40) != 0;
        bmm_ = (d011 & 0x20) != 0;
        mcm_ = (d016 & 0x10) != 0;
        den_ = (d011 & 0x10) != 0;
        rsel_ = (d011 & 0x08) != 0;
        csel_ = (d016 & 0x08) != 0;
        xscroll_ = d016 & 7;
        yscroll_ = d011 & 7;
        border_ = c64.vic(0x20) & 0x0F;
        for (unsigned i = 0; i < 4; ++i) bg_[i] = static_cast<Byte>(c64.vic(0x21 + i) & 0x0F);
        const Byte enabled = c64.vic(0x15);
        for (unsigned i = 0; i < 8; ++i) {
            Sprite& sp = sprites_[i];
            sp.on = (enabled >> i) & 1;
            if (!sp.on) continue;
            int x = c64.vic(2 * i) | (((c64.vic(0x10) >> i) & 1) << 8);
            if (x >= 488) x -= 504;  // PAL: the far right of the coordinate range appears at the left edge
            sp.x = x - 24;
            sp.y = c64.vic(2 * i + 1) - 50;
            sp.xExpand = (c64.vic(0x1D) >> i) & 1;
            sp.yExpand = (c64.vic(0x17) >> i) & 1;
            sp.multicolor = (c64.vic(0x1C) >> i) & 1;
            sp.behind = (c64.vic(0x1B) >> i) & 1;
            sp.color = c64.vic(0x27 + i) & 0x0F;
            const unsigned data = fetch(screen_ + 0x3F8 + i) * 64u;
            bool any = false;
            for (unsigned k = 0; k < 63; ++k) {
                sp.data[k] = fetch(data + k);
                any = any || sp.data[k] != 0;
            }
            sp.on = any;
        }
        mc0_ = c64.vic(0x25) & 0x0F;
        mc1_ = c64.vic(0x26) & 0x0F;
    }

    [[nodiscard]] Byte borderColor() const { return border_; }

    /// Colour of the graphics at a window pixel; `foreground` = the pixel takes part in sprite priority and collisions,
    /// `covered` = the border (or the switched-off display) hides everything here.
    Byte graphics(int x, int y, bool& foreground, bool& covered) const {
        foreground = false;
        covered = true;
        if (!den_) return border_;
        if ((!csel_ && (x < 7 || x >= 311)) || (!rsel_ && (y < 4 || y >= 196))) return border_;
        covered = false;
        const int sx = x - xscroll_, sy = y - (yscroll_ - 3);
        if (sx < 0 || sy < 0 || sx >= kWidth || sy >= kHeight) return bg_[0];
        const unsigned cell = static_cast<unsigned>(sy / 8 * 40 + sx / 8);
        const unsigned line = static_cast<unsigned>(sy % 8);
        const int bit = 7 - sx % 8;
        const Byte code = fetch(screen_ + cell);
        const Byte color = c64_.colorRam(cell) & 0x0F;
        if (ecm_ && (bmm_ || mcm_)) return 0;  // invalid modes show black
        if (!bmm_) {
            if (ecm_) {
                const Byte b = fetch(charBase_ + (code & 0x3Fu) * 8 + line);
                foreground = (b >> bit) & 1;
                return foreground ? color : bg_[code >> 6];
            }
            const Byte b = fetch(charBase_ + code * 8u + line);
            if (!mcm_ || !(color & 0x08)) {
                foreground = (b >> bit) & 1;
                return foreground ? (mcm_ ? static_cast<Byte>(color & 7) : color) : bg_[0];
            }
            const unsigned pair = (b >> (bit & ~1)) & 3;
            foreground = pair >= 2;  // the pairs 00 and 01 count as background
            return pair == 3 ? static_cast<Byte>(color & 7) : bg_[pair];
        }
        const Byte b = fetch(bitmapBase_ + cell * 8 + line);
        if (!mcm_) {
            foreground = (b >> bit) & 1;
            return foreground ? static_cast<Byte>(code >> 4) : static_cast<Byte>(code & 0x0F);
        }
        const unsigned pair = (b >> (bit & ~1)) & 3;
        foreground = pair >= 2;
        switch (pair) {
            case 0: return bg_[0];
            case 1: return static_cast<Byte>(code >> 4);
            case 2: return static_cast<Byte>(code & 0x0F);
            default: return color;
        }
    }

    [[nodiscard]] bool spriteOn(unsigned i) const { return sprites_[i].on; }
    [[nodiscard]] bool spriteBehind(unsigned i) const { return sprites_[i].behind; }
    /// Pixel box of a sprite in window coordinates (may lie partly or completely outside the window).
    void spriteBox(unsigned i, int& x0, int& y0, int& x1, int& y1) const {
        const Sprite& sp = sprites_[i];
        x0 = sp.x;
        y0 = sp.y;
        x1 = sp.x + (sp.xExpand ? 48 : 24) - 1;
        y1 = sp.y + (sp.yExpand ? 42 : 21) - 1;
    }
    /// Does sprite `i` show a pixel at the window position, and in which colour?
    bool spritePixel(unsigned i, int x, int y, Byte& color) const {
        const Sprite& sp = sprites_[i];
        int dx = x - sp.x, dy = y - sp.y;
        if (dx < 0 || dy < 0) return false;
        if (sp.xExpand) dx >>= 1;
        if (sp.yExpand) dy >>= 1;
        if (dx >= 24 || dy >= 21) return false;
        const Byte b = sp.data[static_cast<unsigned>(dy * 3 + dx / 8)];
        const int bit = 7 - dx % 8;
        if (!sp.multicolor) {
            color = sp.color;
            return (b >> bit) & 1;
        }
        switch ((b >> (bit & ~1)) & 3) {
            case 0: return false;
            case 1: color = mc0_; break;
            case 2: color = sp.color; break;
            default: color = mc1_; break;
        }
        return true;
    }

private:
    struct Sprite {
        bool on = false, xExpand = false, yExpand = false, multicolor = false, behind = false;
        int x = 0, y = 0;
        Byte color = 0;
        std::array<Byte, 63> data{};
    };

    // What the VIC reads at a 14-bit address of its bank: the character ROM at $1000-$1FFF in banks 0 and 2.
    [[nodiscard]] Byte fetch(unsigned rel) const {
        rel &= 0x3FFF;
        if (charRomInBank_ && (rel & 0x3000) == 0x1000) {
            const unsigned off = rel & 0x0FFF;
            if (c64_.hasChar()) return c64_.charRom()[off];
            const unsigned code = (off >> 3) & 0xFF;
            const Byte b = replacementCharset((off & 0x800) != 0)[(code & 0x7F) * 8 + (off & 7)];
            return (code & 0x80) ? static_cast<Byte>(~b) : b;
        }
        return ram_.read8(static_cast<Address>(bank_ + rel));
    }

    const C64& c64_;
    const Memory& ram_;
    Address bank_ = 0;
    bool charRomInBank_ = false;
    unsigned screen_ = 0, charBase_ = 0, bitmapBase_ = 0;
    bool ecm_ = false, bmm_ = false, mcm_ = false, den_ = false, rsel_ = false, csel_ = false;
    int xscroll_ = 0, yscroll_ = 0;
    Byte border_ = 0, mc0_ = 0, mc1_ = 0;
    std::array<Byte, 4> bg_{};
    std::array<Sprite, 8> sprites_{};
};

}  // namespace

IndexedImage renderScreen(const C64& c64, const Memory& ram, const RenderOptions& options) {
    const Frame frame(c64, ram);
    IndexedImage img;
    const int bx = options.border ? kBorderX : 0, by = options.border ? kBorderY : 0;
    img.width = kWidth + 2 * bx;
    img.height = kHeight + 2 * by;
    const auto& pal = paletteColors(options.palette);
    img.palette.assign(pal.begin(), pal.end());
    img.pixels.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), frame.borderColor());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            bool foreground = false, covered = false;
            Byte color = frame.graphics(x, y, foreground, covered);
            if (options.sprites && !covered) {
                // the sprite with the lowest number that has a pixel here decides, even if it is behind the graphics
                for (unsigned i = 0; i < 8; ++i) {
                    Byte c = 0;
                    if (!frame.spriteOn(i) || !frame.spritePixel(i, x, y, c)) continue;
                    if (!(frame.spriteBehind(i) && foreground)) color = c;
                    break;
                }
            }
            img.pixels[static_cast<std::size_t>(y + by) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x + bx)] = color;
        }
    }
    return img;
}

SpriteCollisions spriteCollisions(const C64& c64, const Memory& ram) {
    SpriteCollisions out;
    if (c64.vic(0x15) == 0) return out;
    const Frame frame(c64, ram);
    struct Box {
        int x0, y0, x1, y1;
    };
    std::array<Box, 8> box{};
    for (unsigned i = 0; i < 8; ++i)
        if (frame.spriteOn(i)) frame.spriteBox(i, box[i].x0, box[i].y0, box[i].x1, box[i].y1);
    Byte c = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (!frame.spriteOn(i)) continue;
        // sprite - sprite: anywhere, also where the border hides them
        for (unsigned j = i + 1; j < 8; ++j) {
            if (!frame.spriteOn(j)) continue;
            const int x0 = std::max(box[i].x0, box[j].x0), x1 = std::min(box[i].x1, box[j].x1);
            const int y0 = std::max(box[i].y0, box[j].y0), y1 = std::min(box[i].y1, box[j].y1);
            bool hit = false;
            for (int y = y0; y <= y1 && !hit; ++y)
                for (int x = x0; x <= x1 && !hit; ++x) hit = frame.spritePixel(i, x, y, c) && frame.spritePixel(j, x, y, c);
            if (hit) out.spriteSprite = static_cast<Byte>(out.spriteSprite | (1u << i) | (1u << j));
        }
        // sprite - graphics: inside the window, foreground pixels only
        bool hit = false;
        for (int y = std::max(box[i].y0, 0); y <= std::min(box[i].y1, kHeight - 1) && !hit; ++y) {
            for (int x = std::max(box[i].x0, 0); x <= std::min(box[i].x1, kWidth - 1) && !hit; ++x) {
                if (!frame.spritePixel(i, x, y, c)) continue;
                bool foreground = false, covered = false;
                (void)frame.graphics(x, y, foreground, covered);
                hit = foreground && !covered;
            }
        }
        if (hit) out.spriteBackground = static_cast<Byte>(out.spriteBackground | (1u << i));
    }
    return out;
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
