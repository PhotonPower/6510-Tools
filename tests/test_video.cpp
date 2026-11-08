#include "m6510/video.hpp"

#include "m6510/cpu.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

struct Machine64 {
    Memory ram;
    Cpu cpu{ram};
    C64 c64{ram, cpu};
    Machine64() {
        c64.resetChips();
        // what the KERNAL sets up: text mode, 40 columns, 25 rows, screen $0400, upper-case character set
        c64.write(0xD011, 0x1B);
        c64.write(0xD016, 0x08);
        c64.write(0xD018, 0x14);
        c64.write(0xD020, 14);
        c64.write(0xD021, 6);
        c64.write(0xDD02, 0x03);
        c64.write(0xDD00, 0x03);  // bank 0
    }
    IndexedImage render(bool border = false) {
        RenderOptions o;
        o.border = border;
        return renderScreen(c64, ram, o);
    }
};

int px(const IndexedImage& img, int x, int y) {
    return img.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)];
}

// bit pattern of one pixel row (8 pixels) as a string of palette indices in hex
std::string row(const IndexedImage& img, int x, int y) {
    std::string s;
    for (int i = 0; i < 8; ++i) s += hexDigits(static_cast<unsigned>(px(img, x + i, y)), 1);
    return s;
}

}  // namespace

int main() {
    // ---- standard text with the replacement character set ---------------------------------------------
    {
        Machine64 m;
        m.ram.write8(0x0400, 1);  // 'A'
        m.c64.setColorRam(0, 1);
        m.ram.write8(0x0401, 0x81);  // reversed 'A'
        m.c64.setColorRam(1, 2);
        IndexedImage img = m.render();
        CHECK_EQ(img.width, 320);
        CHECK_EQ(img.height, 200);
        CHECK_EQ(img.palette.size(), std::size_t{16});
        const auto& a = replacementCharset(false);
        for (int y = 0; y < 8; ++y) {
            std::string expected, reversed;
            for (int x = 0; x < 8; ++x) {
                const bool on = (a[8 + static_cast<std::size_t>(y)] >> (7 - x)) & 1;
                expected += on ? '1' : '6';
                reversed += on ? '6' : '2';
            }
            CHECK_EQ(row(img, 0, y), expected);
            CHECK_EQ(row(img, 8, y), reversed);
        }
        CHECK_EQ(px(img, 16, 0), 6);  // screen code 0 ('@') in colour 0 = black? colour RAM 0 -> foreground black
        // with border
        img = m.render(true);
        CHECK_EQ(img.width, 384);
        CHECK_EQ(img.height, 272);
        CHECK_EQ(px(img, 0, 0), 14);
        CHECK_EQ(px(img, 383, 271), 14);
        CHECK_EQ(row(img, 32, 36), row(m.render(), 0, 0));
        // the lower-case set ($D018 bit 1) is different for screen code 1
        m.c64.write(0xD018, 0x16);
        CHECK(row(m.render(), 0, 3) != row(img, 32, 39));
        CHECK(replacementCharset(true) != replacementCharset(false));
        // a real character ROM is used when present
        std::vector<Byte> rom(0x1000, 0);
        for (int i = 0; i < 8; ++i) rom[8 + static_cast<std::size_t>(i)] = 0xF0;  // screen code 1: left half
        m.c64.loadRom("char", rom);
        m.c64.write(0xD018, 0x14);
        CHECK_EQ(row(m.render(), 0, 5), std::string("11116666"));
    }

    // ---- display enable, 38 columns, 24 rows, scrolling ------------------------------------------------
    {
        Machine64 m;
        for (unsigned i = 0; i < 1000; ++i) {
            m.ram.write8(static_cast<Address>(0x0400 + i), 0xA0);  // reversed space: solid block
            m.c64.setColorRam(i, 7);
        }
        IndexedImage img = m.render();
        CHECK_EQ(px(img, 0, 0), 7);
        CHECK_EQ(px(img, 319, 199), 7);
        m.c64.write(0xD011, 0x0B);  // display off
        img = m.render();
        CHECK_EQ(px(img, 100, 100), 14);
        m.c64.write(0xD011, 0x1B);
        m.c64.write(0xD016, 0x00);  // 38 columns
        img = m.render();
        CHECK_EQ(px(img, 6, 50), 14);
        CHECK_EQ(px(img, 7, 50), 7);
        CHECK_EQ(px(img, 311, 50), 14);
        CHECK_EQ(px(img, 310, 50), 7);
        m.c64.write(0xD016, 0x08);
        m.c64.write(0xD011, 0x13);  // 24 rows
        img = m.render();
        CHECK_EQ(px(img, 50, 3), 14);
        CHECK_EQ(px(img, 50, 4), 7);
        CHECK_EQ(px(img, 50, 196), 14);
        m.c64.write(0xD011, 0x1B);
        m.c64.write(0xD016, 0x0A);  // x scroll 2: two background pixels on the left
        img = m.render();
        CHECK_EQ(px(img, 1, 50), 6);
        CHECK_EQ(px(img, 2, 50), 7);
        m.c64.write(0xD016, 0x08);
        m.c64.write(0xD011, 0x1C);  // y scroll 4: one line lower than normal (3)
        img = m.render();
        CHECK_EQ(px(img, 50, 0), 6);
        CHECK_EQ(px(img, 50, 1), 7);
    }

    // ---- multicolour text, extended background, invalid mode ------------------------------------------
    {
        Machine64 m;
        m.c64.write(0xD018, 0x1C);  // charset at $3000 (RAM)
        m.c64.write(0xD022, 2);
        m.c64.write(0xD023, 3);
        m.c64.write(0xD024, 4);
        m.ram.write8(0x3000, 0x1B);  // char 0, row 0: 00 01 10 11
        m.ram.write8(0x3008, 0x81);  // char 1, row 0: 1000 0001
        m.ram.write8(0x0400, 0);
        m.c64.setColorRam(0, 0x0D);  // bit 3 set: multicolour, colour 5
        m.ram.write8(0x0401, 1);
        m.c64.setColorRam(1, 0x05);  // bit 3 clear: hires in colour 5
        m.c64.write(0xD016, 0x18);
        IndexedImage img = m.render();
        CHECK_EQ(row(img, 0, 0), std::string("66223355"));
        CHECK_EQ(row(img, 8, 0), std::string("56666665"));  // colour bit 3 clear: hires in colour 5
        // extended background colour: screen code bits 6-7 choose $D021-$D024
        m.c64.write(0xD016, 0x08);
        m.c64.write(0xD011, 0x5B);
        m.ram.write8(0x0400, 0xC1);  // char 1 with background 3 ($D024 = 4)
        m.c64.setColorRam(0, 1);
        img = m.render();
        CHECK_EQ(row(img, 0, 0), std::string("14444441"));
        m.ram.write8(0x0400, 0x41);  // background 1 ($D022 = 2)
        CHECK_EQ(row(m.render(), 0, 0), std::string("12222221"));
        m.c64.write(0xD016, 0x18);  // ECM + MCM: invalid, black
        CHECK_EQ(row(m.render(), 0, 0), std::string("00000000"));
    }

    // ---- bitmap modes ----------------------------------------------------------------------------------
    {
        Machine64 m;
        m.c64.write(0xD011, 0x3B);
        m.c64.write(0xD018, 0x18);  // screen $0400, bitmap $2000
        m.ram.write8(0x2000, 0x81);
        m.ram.write8(0x0400, 0x25);  // pixel set: colour 2, clear: colour 5
        IndexedImage img = m.render();
        CHECK_EQ(row(img, 0, 0), std::string("25555552"));
        m.ram.write8(0x2001 + 8 * 41 - 1 + 1, 0xFF);  // cell 41 (row 1, column 1), line 1
        m.ram.write8(0x0400 + 41, 0x70);
        CHECK_EQ(row(m.render(), 8, 9), std::string("77777777"));
        // multicolour bitmap: 00 = $D021, 01 = screen high nybble, 10 = screen low nybble, 11 = colour RAM
        m.c64.write(0xD016, 0x18);
        m.ram.write8(0x2000, 0x1B);
        m.c64.setColorRam(0, 9);
        CHECK_EQ(row(m.render(), 0, 0), std::string("66225599"));
    }

    // ---- VIC bank ----------------------------------------------------------------------------------------
    {
        Machine64 m;
        m.c64.write(0xDD00, 0x02);  // bank 1: $4000-$7FFF, no character ROM there
        CHECK_EQ(m.c64.vicBank(), Address{0x4000});
        m.c64.write(0xD018, 0x14);  // screen $4400, charset $5000 (RAM in this bank)
        m.ram.write8(0x4400, 3);
        m.ram.write8(0x5018, 0xAA);  // char 3, row 0
        m.c64.setColorRam(0, 1);
        CHECK_EQ(row(m.render(), 0, 0), std::string("16161616"));
        m.c64.write(0xDD00, 0x01);  // bank 2: $8000, character ROM at $9000
        m.c64.write(0xD018, 0x04);  // screen $8000, charset $9000 = ROM (replacement)
        m.ram.write8(0x8000, 0x20);  // space
        CHECK_EQ(row(m.render(), 0, 0), std::string("66666666"));
    }

    // ---- crop, zoom, diff --------------------------------------------------------------------------------
    {
        Machine64 m;
        m.ram.write8(0x0400, 0xA0);
        m.c64.setColorRam(0, 2);
        const IndexedImage full = m.render();
        const IndexedImage z = cropAndZoom(full, 0, 0, 7, 7, 4);
        CHECK_EQ(z.width, 32);
        CHECK_EQ(z.height, 32);
        CHECK_EQ(px(z, 31, 31), 2);
        const IndexedImage c = cropAndZoom(full, 300, 190, 400, 400, 1);  // clamped
        CHECK_EQ(c.width, 20);
        CHECK_EQ(c.height, 10);
        ImageDiff d = diffImages(full, full);
        CHECK_EQ(d.changedPixels, std::uint64_t{0});
        m.ram.write8(0x0400 + 40 * 2 + 3, 0xA0);
        m.c64.setColorRam(40 * 2 + 3, 1);
        d = diffImages(full, m.render());
        CHECK_EQ(d.changedPixels, std::uint64_t{64});
        CHECK_EQ(d.x0, 24);
        CHECK_EQ(d.y0, 16);
        CHECK_EQ(d.x1, 31);
        CHECK_EQ(d.y1, 23);
        CHECK(!diffImages(full, z).sameSize);
    }

    // ---- palettes, PNG, Base64 ------------------------------------------------------------------------------
    {
        Palette p = Palette::Colodore;
        CHECK(parsePalette("pepto", p));
        CHECK(p == Palette::Pepto);
        CHECK(!parsePalette("vice", p));
        CHECK_EQ(paletteColors(Palette::Colodore)[1], std::uint32_t{0xFFFFFF});
        CHECK(paletteColors(Palette::Colodore)[2] != paletteColors(Palette::Pepto)[2]);
        IndexedImage img;
        img.width = 3;
        img.height = 2;
        img.palette = {0x000000, 0xFF0000};
        img.pixels = {0, 1, 0, 1, 0, 1};
        const std::vector<Byte> png = encodePng(img);
        CHECK_EQ(hexBytes(std::vector<Byte>(png.begin(), png.begin() + 8)), std::string("89 50 4E 47 0D 0A 1A 0A"));
        CHECK_EQ(hexBytes(std::vector<Byte>(png.begin() + 12, png.begin() + 24)), std::string("49 48 44 52 00 00 00 03 00 00 00 02"));
        CHECK_EQ(hexBytes(std::vector<Byte>(png.end() - 8, png.end())), std::string("AE 42 60 82").insert(0, "49 45 4E 44 "));
        img.pixels.pop_back();
        bool threw = false;
        try {
            (void)encodePng(img);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        CHECK_EQ(base64Encode({}), std::string());
        CHECK_EQ(base64Encode({'M'}), std::string("TQ=="));
        CHECK_EQ(base64Encode({'M', 'a'}), std::string("TWE="));
        CHECK_EQ(base64Encode({'M', 'a', 'n'}), std::string("TWFu"));
    }

    return finish("video");
}
