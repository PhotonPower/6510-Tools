#include <cstdio>
#include <filesystem>
#include <random>

#include "image_builder.hpp"
#include "m6510/d64.hpp"
#include "m6510/image_io.hpp"
#include "m6510/monitor.hpp"
#include "m6510/session.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

std::vector<Byte> pattern(std::size_t n, unsigned seed) {
    std::vector<Byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<Byte>((i * 7 + seed) & 0xFF);
    return v;
}

std::string tempFile(const std::string& name) { return (std::filesystem::temp_directory_path() / name).string(); }

Json call(Session& s, const std::string& line) { return Json::parse(s.handleLine(line)); }

}  // namespace

int main() {
    // ---- a disk with a few files ------------------------------------------------------------------------------------
    const std::vector<Byte> big = makePrg(0x0801, pattern(600, 3));  // 602 bytes: 3 blocks
    const std::vector<Byte> one = makePrg(0xC000, {0x60});           // 3 bytes
    const std::vector<Byte> seq = pattern(254, 1);                   // exactly one full block
    testimg::D64Builder b;
    b.addFile("GAME", big);
    b.addFile("NOTES", seq, 0x81);  // SEQ
    b.addFile("TOOL", one);
    b.addFile("LOCKED", one, 0xC2);  // PRG, locked
    b.addFile("OPEN", one, 0x02);    // PRG, not closed
    const std::vector<Byte> image = b.build();
    {
        const D64Image disk(image);
        CHECK_EQ(disk.tracks(), 35U);
        CHECK(!disk.hasErrorBytes());
        CHECK_EQ(disk.diskName(), std::string("TEST DISK"));
        CHECK_EQ(disk.diskId(), std::string("AB"));
        CHECK_EQ(disk.dosType(), std::string("2A"));
        CHECK_EQ(disk.freeBlocks(), 664U - 7U);  // 683 - 19 (track 18) - 7 blocks of files
        const std::vector<D64Entry> dir = disk.directory();
        CHECK_EQ(dir.size(), std::size_t{5});
        CHECK_EQ(dir[0].name, std::string("GAME"));
        CHECK_EQ(dir[0].type, std::string("PRG"));
        CHECK_EQ(dir[0].blocks, 3U);
        CHECK_EQ(dir[1].type, std::string("SEQ"));
        CHECK(dir[3].locked);
        CHECK(!dir[4].closed);
        CHECK((disk.readFile(dir[0])) == (big));
        CHECK((disk.readFile(dir[1])) == (seq));
        CHECK((disk.readFile(dir[2])) == (one));
        const Program p = disk.readProgram(dir[0]);
        CHECK_EQ(p.address, Address{0x0801});
        CHECK_EQ(p.bytes.size(), std::size_t{600});
        CHECK(disk.problems().empty());
        // patterns
        CHECK_EQ(disk.find("*")->name, std::string("GAME"));
        CHECK_EQ(disk.find("t*")->name, std::string("TOOL"));
        CHECK_EQ(disk.find("?OTES")->name, std::string("NOTES"));
        CHECK(!disk.find("NOPE"));
        CHECK(!disk.find("GAM"));
        CHECK(cbmNameMatches("G*", "GAME"));
        CHECK(cbmNameMatches("GA?E", "game"));
        CHECK(!cbmNameMatches("GA?", "GAME"));
        CHECK(cbmNameMatches("*", ""));
        // blocks
        const std::vector<Byte> bam = disk.block(18, 0);
        CHECK_EQ(bam[0], Byte{18});
        CHECK(disk.validBlock(17, 20));
        CHECK(!disk.validBlock(18, 19));
        CHECK(!disk.validBlock(36, 0));
        bool threw = false;
        try {
            (void)disk.block(0, 0);
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(!disk.errorCode(1, 0));
    }

    // ---- 40 tracks, error bytes, a directory over several sectors ---------------------------------------------------
    {
        testimg::D64Builder b40(40, true);
        for (int i = 0; i < 12; ++i) b40.addFile("FILE" + std::to_string(i), makePrg(0x1000, pattern(10 + static_cast<std::size_t>(i), static_cast<unsigned>(i))));
        b40.setError(1, 0, 0x05);
        const D64Image disk(b40.build());
        CHECK_EQ(disk.tracks(), 40U);
        CHECK(disk.hasErrorBytes());
        CHECK_EQ(disk.badSectors(), 1U);
        CHECK_EQ(static_cast<unsigned>(*disk.errorCode(1, 0)), 5U);
        CHECK_EQ(static_cast<unsigned>(*disk.errorCode(40, 16)), 1U);
        const auto dir = disk.directory();
        CHECK_EQ(dir.size(), std::size_t{12});
        CHECK_EQ(dir[11].name, std::string("FILE11"));
        CHECK((disk.readProgram(*disk.find("FILE11")).bytes) == (pattern(21, 11)));
    }

    // ---- damaged images ------------------------------------------------------------------------------------------------
    {
        bool threw = false;
        try {
            D64Image bad(std::vector<Byte>(1000, 0));
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
        // a file chain that loops, one that leaves the disk, a wrong block count, a directory that loops
        testimg::D64Builder bb;
        const auto first = bb.addFile("LOOP", makePrg(0x1000, pattern(600, 1)));
        const auto second = bb.addFile("WILD", makePrg(0x1000, pattern(300, 2)));
        std::vector<Byte> img = bb.build();
        testimg::D64Builder ref;  // only to compute offsets
        auto at = [&](unsigned t, unsigned s) { return img.data() + ref.offset(t, s); };
        // LOOP: make its second block point back to the first
        Byte* b1 = at(first.first, first.second);
        Byte* b2 = at(b1[0], b1[1]);
        b2[0] = static_cast<Byte>(first.first);
        b2[1] = static_cast<Byte>(first.second);
        // WILD: first block points to track 50
        Byte* w = at(second.first, second.second);
        w[0] = 50;
        const D64Image disk(img);
        for (const char* name : {"LOOP", "WILD"}) {
            threw = false;
            try {
                (void)disk.readFile(*disk.find(name));
            } catch (const IoError& e) {
                threw = true;
                CHECK(std::string(e.what()).find(name) != std::string::npos);
            }
            CHECK(threw);
        }
        // the directory claims a wrong size: read, with a problem noted
        testimg::D64Builder bs;
        bs.addFile("SIZE", makePrg(0x1000, pattern(10, 1)));
        img = bs.build();
        at(18, 1)[2 + 28] = 9;  // blocks low byte of entry 0 at offset 30
        const D64Image d2(img);
        (void)d2.readFile(d2.directory()[0]);
        CHECK(!d2.problems().empty());
        // directory loop
        at(18, 1)[0] = 18;
        at(18, 1)[1] = 1;
        const D64Image d3(img);
        CHECK_EQ(d3.directory().size(), std::size_t{1});
        CHECK(!d3.problems().empty());
    }

    // ---- random mutations: only IoError, never a crash or another exception ---------------------------------------------
    {
        std::mt19937 gen(64);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        int ioErrors = 0;
        for (int iter = 0; iter < 400; ++iter) {
            std::vector<Byte> img = image;
            const unsigned n = 1 + rng() % 40;
            for (unsigned k = 0; k < n; ++k) {
                // mostly hit the directory track and the file blocks, sometimes anywhere
                const std::size_t pos = rng() % 3 == 0 ? rng() % img.size() : 0x16500 + rng() % (19 * 256);
                img[pos % img.size()] = static_cast<Byte>(rng());
            }
            if (rng() % 10 == 0) img[0] = static_cast<Byte>(rng());
            try {
                const D64Image disk(img);
                (void)disk.diskName();
                (void)disk.freeBlocks();
                for (const D64Entry& e : disk.directory()) {
                    try {
                        (void)disk.readFile(e);
                    } catch (const IoError&) {
                        ++ioErrors;
                    }
                }
            } catch (const IoError&) {
                ++ioErrors;
            } catch (const std::exception& e) {
                CHECK(false);
                std::cerr << "unexpected exception: " << e.what() << "\n";
            }
        }
        CHECK(ioErrors > 0);
    }

    // ---- programs out of a disk: loadProgramSpec, dis-style specs, the session, the monitor, LOAD through the KERNAL ---------
    {
        const std::string path = tempFile("m6510_test_disk.d64");
        writeBinaryFile(path, image);
        CHECK(splitImageSpec(path).has_value());
        CHECK_EQ(splitImageSpec(path + ":TOOL")->second, std::string("TOOL"));
        CHECK(!splitImageSpec("x.prg").has_value());
        CHECK(!splitImageSpec("C:\\games\\x.prg").has_value());
        std::string where;
        Program p = loadProgramSpec(path + ":TOOL", &where);
        CHECK_EQ(p.address, Address{0xC000});
        CHECK(where.find("\"TOOL\" (PRG, 1 blocks)") != std::string::npos);
        p = loadProgramSpec(path);  // the first file
        CHECK_EQ(p.address, Address{0x0801});
        bool threw = false;
        try {
            (void)loadProgramSpec(path + ":NOTES");  // a SEQ file
        } catch (const IoError& e) {
            threw = std::string(e.what()).find("SEQ") != std::string::npos;
        }
        CHECK(threw);
        threw = false;
        try {
            (void)loadProgramSpec(path + ":NOPE");
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);

        SessionOptions so;
        so.allowFiles = true;
        Session s(so);
        call(s, R"({"cmd":"machine","type":"c64"})");
        Json r = call(s, R"({"cmd":"load_file","file":")" + path + R"(","name":"T*"})");
        CHECK(r.find("ok")->asBool());
        CHECK_EQ(r.find("start")->asString(), std::string("0xC000"));
        CHECK(r.find("source")->asString().find("TOOL") != std::string::npos);
        r = call(s, R"({"cmd":"load_file","file":")" + path + R"(:GAME"})");
        CHECK_EQ(r.find("start")->asString(), std::string("0x0801"));
        // LOAD"TOOL",8,1 through the KERNAL emulation from the attached disk
        CHECK(call(s, R"({"cmd":"attach","file":")" + path + R"("})").find("ok")->asBool());
        call(s, R"({"cmd":"write","addr":"0xC000","hex":"00"})");
        call(s, R"({"cmd":"asm","origin":"0x2000","source":"go: LDA #4\n LDX #<n\n LDY #>n\n JSR SETNAM\n LDA #1\n LDX #8\n LDY #1\n JSR SETLFS\n LDA #0\n JSR LOAD\n RTS\nn: .BYTE \"TOOL\""})");
        r = call(s, R"({"cmd":"run_program","addr":"go"})");
        CHECK_EQ(call(s, R"({"cmd":"read","addr":"0xC000","len":1})").find("hex")->asString(), std::string("60"));

        Monitor m;
        const std::string out = m.execute("l \"" + path + ":TOOL\"");
        CHECK(out.find("ok: C000-C000 (1 bytes), PC=C000") != std::string::npos);
        CHECK(out.find("from ") != std::string::npos);
        std::remove(path.c_str());
    }

    return finish("d64");
}
