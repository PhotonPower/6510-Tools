#include "m6510/image_io.hpp"

#include <cstdio>

#include "test_util.hpp"

using namespace m6510;

int main() {
    // PRG
    const std::vector<Byte> prg = makePrg(0x0801, {0x01, 0x02, 0x03});
    CHECK_EQ(hexBytes(prg), std::string("01 08 01 02 03"));
    {
        const Program p = parsePrg(prg);
        CHECK_EQ(p.address, Address{0x0801});
        CHECK_EQ(p.bytes.size(), std::size_t{3});
        CHECK_EQ(p.end(), std::uint32_t{0x0804});
    }
    {
        const Program p = parsePrg({0x00, 0xC0});  // load address only
        CHECK_EQ(p.address, Address{0xC000});
        CHECK(p.bytes.empty());
    }
    bool threw = false;
    try {
        (void)parsePrg({0x01});
    } catch (const IoError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)parsePrg({0xFE, 0xFF, 1, 2, 3});  // runs past $FFFF
    } catch (const IoError&) {
        threw = true;
    }
    CHECK(threw);
    {
        const Program p = parsePrg({0xFE, 0xFF, 1, 2});  // exactly up to $FFFF
        CHECK_EQ(p.end(), std::uint32_t{0x10000});
    }

    // P00
    {
        std::vector<Byte> p00 = {'C', '6', '4', 'F', 'i', 'l', 'e', 0};
        const char name[] = "HELLO";
        for (int i = 0; i < 17; ++i) p00.push_back(i < 5 ? static_cast<Byte>(name[i]) : 0);
        p00.push_back(0);
        p00.insert(p00.end(), prg.begin(), prg.end());
        CHECK(looksLikeP00(p00));
        CHECK(!looksLikeP00(prg));
        const Program p = parseP00(p00);
        CHECK_EQ(p.name, std::string("HELLO"));
        CHECK_EQ(p.address, Address{0x0801});
        CHECK_EQ(hexBytes(p.bytes), std::string("01 02 03"));
        p00[3] = 'X';
        threw = false;
        try {
            (void)parseP00(p00);
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
    }

    // files
    {
        const std::string path = "m6510_test_image_io.prg";
        writeBinaryFile(path, prg);
        CHECK(readBinaryFile(path) == prg);
        const Program p = loadProgramFile(path);
        CHECK_EQ(p.address, Address{0x0801});
        Memory m;
        loadProgram(m, p);
        CHECK_EQ(m.read8(0x0803), Byte{0x03});
        writeTextFile(path, "text\n");
        CHECK_EQ(readTextFile(path), std::string("text\n"));
        std::remove(path.c_str());
        threw = false;
        try {
            (void)readBinaryFile("m6510_does_not_exist.prg");
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
    }

    CHECK_EQ(fileExtension("dir/x.PRG"), std::string(".prg"));
    CHECK_EQ(fileExtension("dir.d/x"), std::string(""));
    CHECK_EQ(fileExtension("a.tar.gz"), std::string(".gz"));

    return finish("image_io");
}
