#include <cstdio>
#include <filesystem>
#include <random>

#include "image_builder.hpp"
#include "m6510/crt.hpp"
#include "m6510/image_io.hpp"
#include "m6510/machine.hpp"
#include "m6510/session.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

std::string tempFile(const std::string& name) { return (std::filesystem::temp_directory_path() / name).string(); }

// An 8K ROM with the "CBM80" signature: cold start vector $8009, code there: INC $D020 / JMP *-3
std::vector<Byte> cbm80Rom() {
    std::vector<Byte> rom(0x2000, 0xEA);
    const Byte head[] = {0x09, 0x80, 0x09, 0x80, 0xC3, 0xC2, 0xCD, 0x38, 0x30, 0xEE, 0x20, 0xD0, 0x4C, 0x09, 0x80};
    std::copy(std::begin(head), std::end(head), rom.begin());
    return rom;
}

bool throwsIoError(const std::vector<Byte>& bytes) {
    try {
        CrtImage c(bytes);
    } catch (const IoError&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    // ---- reading ---------------------------------------------------------------------------------------------------------
    const std::vector<Byte> eightK = testimg::buildCrt(0, false, true, {{0, 0x8000, cbm80Rom()}}, "EIGHT K");
    {
        const CrtImage c(eightK);
        CHECK_EQ(c.name(), std::string("EIGHT K"));
        CHECK_EQ(c.hardwareType(), 0U);
        CHECK_EQ(c.hardwareName(), std::string("normal cartridge"));
        CHECK_EQ(c.mode(), std::string("8K"));
        CHECK_EQ(c.chips().size(), std::size_t{1});
        CHECK_EQ(c.chips()[0].load, Address{0x8000});
        CHECK_EQ(c.chips()[0].data.size(), std::size_t{0x2000});
        CHECK_EQ(CrtImage(testimg::buildCrt(5, false, false, {{0, 0x8000, {1}}, {1, 0x8000, {2}}})).hardwareName(), std::string("Ocean type 1"));
        CHECK_EQ(CrtImage(testimg::buildCrt(99, true, false, {{0, 0xE000, {1}}})).mode(), std::string("Ultimax"));
        CHECK_EQ(CrtImage(testimg::buildCrt(32, false, false, {{0, 0x8000, {1}}})).hardwareName(), std::string("EasyFlash"));
        CHECK_EQ(CrtImage(testimg::buildCrt(77, true, true, {{0, 0x8000, {1}}})).hardwareName(), std::string("type 77"));
    }

    // ---- damaged -------------------------------------------------------------------------------------------------------------
    {
        CHECK(throwsIoError({}));
        CHECK(throwsIoError(std::vector<Byte>(0x40, 0)));
        CHECK(throwsIoError(testimg::buildCrt(0, false, true, {})));  // no CHIP packet
        std::vector<Byte> c = eightK;
        c.resize(c.size() - 1);  // the packet is longer than the file
        CHECK(throwsIoError(c));
        c = eightK;
        c[0x40] = 'X';  // no CHIP signature
        CHECK(throwsIoError(c));
        c = eightK;
        c[0x13] = 0x10;  // header length below 0x40
        CHECK(throwsIoError(c));
        std::mt19937 gen(64);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        for (int iter = 0; iter < 400; ++iter) {
            std::vector<Byte> m = eightK;
            for (unsigned k = 0; k < 1 + rng() % 6; ++k) m[rng() % 0x60] = static_cast<Byte>(rng());
            if (rng() % 5 == 0) m.resize(rng() % m.size());
            try {
                const CrtImage img(m);
                for (const CrtChip& chip : img.chips()) CHECK(chip.load + chip.data.size() <= 0x10000);
            } catch (const IoError&) {
            } catch (const std::exception& e) {
                CHECK(false);
                std::cerr << "unexpected exception: " << e.what() << "\n";
            }
        }
    }

    // ---- 8K cartridge in the machine: ROML at $8000 with $01 = $37, RAM with LORAM off; the cold start vector ---------------
    {
        const std::string path = tempFile("m6510_test_8k.crt");
        writeBinaryFile(path, eightK);
        Machine m;
        m.select(true, true);
        m.memory().write8(0x8000, 0x11);  // RAM underneath
        const LoadedFile f = m.loadFile(path);
        CHECK_EQ(f.entry, Address{0x8009});
        CHECK(f.source.find("8K cartridge") != std::string::npos);
        CHECK_EQ(m.c64()->cartridgeMode(), std::string("8K"));
        CHECK_EQ(m.peek(0x8004), Byte{0xC3});
        CHECK(m.c64()->describeConfig().find("$8000:ROML") != std::string::npos);
        m.poke(0x8000, 0x22);                // writes reach the RAM underneath
        CHECK_EQ(m.memory().read8(0x8000), Byte{0x22});
        m.poke(1, 0x36);                     // LORAM off: RAM
        CHECK_EQ(m.peek(0x8000), Byte{0x22});
        m.poke(1, 0x37);
        const RunResult r = m.cpu().run(10);
        CHECK_EQ(r.instructions, std::uint64_t{10});
        CHECK_EQ(m.c64()->takeIoLog().size(), std::size_t{10});  // INC $D020 five times (two writes each)
        std::remove(path.c_str());
    }

    // ---- 16K: ROMH at $A000 instead of BASIC; Ultimax: ROMH at $E000 with the reset vector, open areas ------------------------
    {
        std::vector<Byte> roml = cbm80Rom(), romh(0x2000, 0x42);
        const std::string path = tempFile("m6510_test_16k.crt");
        writeBinaryFile(path, testimg::buildCrt(0, false, false, {{0, 0x8000, roml}, {0, 0xA000, romh}}));
        Machine m;
        m.select(true, true);
        (void)m.loadFile(path);
        CHECK_EQ(m.c64()->cartridgeMode(), std::string("16K"));
        CHECK_EQ(m.peek(0xA123), Byte{0x42});
        CHECK(m.c64()->describeConfig().find("$A000:ROMH") != std::string::npos);
        // a single 16K chip at $8000 is split into ROML and ROMH
        std::vector<Byte> both(0x4000, 0x33);
        std::copy(roml.begin(), roml.end(), both.begin());
        writeBinaryFile(path, testimg::buildCrt(0, false, false, {{0, 0x8000, both}}));
        (void)m.loadFile(path);
        CHECK_EQ(m.peek(0xA000), Byte{0x33});

        std::vector<Byte> ultimax(0x2000, 0xEA);
        ultimax[0x1FFC] = 0x00;  // reset vector $E000
        ultimax[0x1FFD] = 0xE0;
        writeBinaryFile(path, testimg::buildCrt(0, true, false, {{0, 0xE000, ultimax}}));
        const LoadedFile f = m.loadFile(path);
        CHECK_EQ(m.c64()->cartridgeMode(), std::string("Ultimax"));
        CHECK_EQ(f.entry, Address{0xE000});
        CHECK_EQ(m.peek(0xE000), Byte{0xEA});
        CHECK_EQ(m.peek(0x2000), Byte{0xFF});  // not connected
        m.poke(0x2000, 0x12);
        CHECK_EQ(m.memory().read8(0x2000), Byte{0x00});  // writes go nowhere
        m.poke(0x0400, 0x12);
        CHECK_EQ(m.peek(0x0400), Byte{0x12});  // the first 4 KiB are RAM
        CHECK(!m.c64()->kernalVisible());

        // a bank-switching cartridge is refused for running, through the session as well
        writeBinaryFile(path, testimg::buildCrt(5, false, false, {{0, 0x8000, roml}, {1, 0x8000, roml}}));
        bool threw = false;
        try {
            (void)m.loadFile(path);
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("Ocean type 1") != std::string::npos;
        }
        CHECK(threw);
        SessionOptions so;
        so.allowFiles = true;
        Session s(so);
        CHECK(!Json::parse(s.handleLine(R"({"cmd":"load_file","file":")" + path + R"("})")).find("ok")->asBool());  // generic machine
        std::remove(path.c_str());
    }

    return finish("crt");
}
