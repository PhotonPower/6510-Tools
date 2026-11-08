#include <cstdio>
#include <filesystem>
#include <random>

#include "image_builder.hpp"
#include "m6510/image_io.hpp"
#include "m6510/t64.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

std::vector<Byte> pattern(std::size_t n, unsigned seed) {
    std::vector<Byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<Byte>((i * 13 + seed) & 0xFF);
    return v;
}

bool throwsIoError(const std::vector<Byte>& bytes) {
    try {
        T64Image t(bytes);
    } catch (const IoError&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    // ---- a normal archive ------------------------------------------------------------------------------------------
    const std::vector<Byte> a = pattern(300, 1), c = pattern(40, 2);
    const std::vector<Byte> tape = testimg::buildT64({{"FIRST", 0x0801, a, -1}, {"SECOND", 0xC000, c, -1}});
    {
        const T64Image t(tape);
        CHECK_EQ(t.tapeName(), std::string("TEST TAPE"));
        CHECK_EQ(t.version(), 0x0100U);
        CHECK_EQ(t.entries().size(), std::size_t{2});
        CHECK_EQ(t.entries()[0].name, std::string("FIRST"));
        CHECK_EQ(t.entries()[1].start, Address{0xC000});
        CHECK_EQ(t.entries()[1].length, 40U);
        CHECK(t.problems().empty());
        const Program p = t.readProgram(*t.find("SEC*"));
        CHECK_EQ(p.address, Address{0xC000});
        CHECK((p.bytes) == (c));
        CHECK_EQ(t.find("*")->name, std::string("FIRST"));
        CHECK(!t.find("THIRD"));
    }

    // ---- the common faults: a wrong end address, 0 used entries, free slots -----------------------------------------
    {
        const T64Image t(testimg::buildT64({{"BROKEN", 0x0801, a, 0xC3C6}}, "X", 0, 3));
        CHECK_EQ(t.entries().size(), std::size_t{1});
        CHECK_EQ(t.entries()[0].length, 300U);  // taken from the file size
        CHECK((t.readProgram(t.entries()[0]).bytes) == (a));
        CHECK_EQ(t.problems().size(), std::size_t{2});  // the end address and the used-entries count
        const T64Image short_end(testimg::buildT64({{"SHORT", 0x0801, a, 0x0810}}));
        CHECK_EQ(short_end.entries()[0].length, 15U);  // a smaller end address is believed
    }

    // ---- not a T64, damaged ---------------------------------------------------------------------------------------------
    {
        CHECK(throwsIoError({}));
        CHECK(throwsIoError(std::vector<Byte>(64, 'x')));
        std::vector<Byte> t = tape;
        t[0x22] = 200;  // more directory entries than the file holds
        CHECK(throwsIoError(t));
        t = tape;
        t[0x22] = 0;
        t[0x23] = 0;
        CHECK(throwsIoError(t));
        t = tape;
        t[64 + 8 + 3] = 0x7F;  // the data offset of the first file far behind the end
        const T64Image skip(t);
        CHECK_EQ(skip.entries().size(), std::size_t{1});
        CHECK(!skip.problems().empty());
    }

    // ---- random mutations ---------------------------------------------------------------------------------------------------
    {
        std::mt19937 gen(64);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        for (int iter = 0; iter < 500; ++iter) {
            std::vector<Byte> t = tape;
            for (unsigned k = 0; k < 1 + rng() % 8; ++k) t[rng() % 160] = static_cast<Byte>(rng());
            if (rng() % 4 == 0) t.resize(rng() % t.size());
            try {
                const T64Image img(t);
                for (const T64Entry& e : img.entries()) {
                    const Program p = img.readProgram(e);
                    CHECK(p.bytes.size() == e.length);
                    CHECK(e.start + e.length <= 0x10000);
                }
            } catch (const IoError&) {
            } catch (const std::exception& e) {
                CHECK(false);
                std::cerr << "unexpected exception: " << e.what() << "\n";
            }
        }
    }

    // ---- loadProgramSpec ----------------------------------------------------------------------------------------------------
    {
        const std::string path = (std::filesystem::temp_directory_path() / "m6510_test_tape.t64").string();
        writeBinaryFile(path, tape);
        std::string where;
        const Program p = loadProgramSpec(path + ":SECOND", &where);
        CHECK_EQ(p.address, Address{0xC000});
        CHECK(where.find("\"SECOND\" (40 bytes)") != std::string::npos);
        CHECK_EQ(loadProgramSpec(path).address, Address{0x0801});
        std::remove(path.c_str());
    }

    return finish("t64");
}
