#include "m6510/petscii.hpp"

#include "test_util.hpp"

using namespace m6510;

int main() {
    // ASCII -> PETSCII (.PETSCII)
    CHECK(asciiToPetscii('a') == std::optional<Byte>(0x41));
    CHECK(asciiToPetscii('z') == std::optional<Byte>(0x5A));
    CHECK(asciiToPetscii('A') == std::optional<Byte>(0xC1));
    CHECK(asciiToPetscii('0') == std::optional<Byte>(0x30));
    CHECK(asciiToPetscii(' ') == std::optional<Byte>(0x20));
    CHECK(asciiToPetscii('@') == std::optional<Byte>(0x40));
    CHECK(!asciiToPetscii('{').has_value());
    CHECK(!asciiToPetscii('~').has_value());

    // ASCII -> screen codes (.SCREEN)
    CHECK(asciiToScreenCode('@') == std::optional<Byte>(0x00));
    CHECK(asciiToScreenCode('a') == std::optional<Byte>(0x01));
    CHECK(asciiToScreenCode('z') == std::optional<Byte>(0x1A));
    CHECK(asciiToScreenCode('A') == std::optional<Byte>(0x41));
    CHECK(asciiToScreenCode(' ') == std::optional<Byte>(0x20));
    CHECK(asciiToScreenCode('9') == std::optional<Byte>(0x39));
    CHECK(!asciiToScreenCode('`').has_value());

    // PETSCII -> text
    CHECK_EQ(petsciiToText(0x41), std::string("A"));
    CHECK_EQ(petsciiToText(0x93), std::string("{CLR}"));
    CHECK_EQ(petsciiToText(0x1C), std::string("{RED}"));
    CHECK_EQ(petsciiToText(0xC1), std::string("{$C1}"));

    // tokens
    CHECK_EQ(basicToken(0x80), std::string("END"));
    CHECK_EQ(basicToken(0x99), std::string("PRINT"));
    CHECK_EQ(basicToken(0x9E), std::string("SYS"));
    CHECK_EQ(basicToken(0xCB), std::string("GO"));
    CHECK_EQ(basicToken(0xCC), std::string(""));
    CHECK_EQ(basicToken(0x41), std::string(""));

    // BASIC starter
    const std::vector<Byte> stub = makeBasicStub(0x0801, 10, 2061);
    CHECK_EQ(hexBytes(stub), std::string("0B 08 0A 00 9E 32 30 36 31 00 00 00"));
    Memory m;
    m.load(0x0801, stub);
    {
        const auto s = parseSysStub(m, 0x0801);
        CHECK(s.has_value());
        CHECK_EQ(s->line, 10U);
        CHECK_EQ(s->target, Address{2061});
        CHECK_EQ(s->length, std::size_t{12});
        CHECK(findSysTarget(m, 0x0801) == std::optional<Address>(2061));
    }

    // detokenise: two lines, quotes and REM keep their bytes
    {
        Memory p;
        // 10 PRINT"{CLR}HI":REM PRINT    20 SYS 4096
        const std::vector<Byte> prog = {
            0x16, 0x08, 0x0A, 0x00, 0x99, 0x22, 0x93, 0x48, 0x49, 0x22, 0x3A, 0x8F, 0x20, 0x99, 0x00,  // line 10 (until $0815)
            0x00, 0x00};
        // fix up: line 10 occupies $0801..$080F (15 bytes) -> next line at $0810
        std::vector<Byte> program = prog;
        program[0] = 0x10;
        program[1] = 0x08;
        program.resize(15);
        // line 20 at $0810: link $081B, 20, SYS, " 4096", 0
        const std::vector<Byte> line20 = {0x1B, 0x08, 0x14, 0x00, 0x9E, 0x20, 0x34, 0x30, 0x39, 0x36, 0x00, 0x00, 0x00};
        program.insert(program.end(), line20.begin(), line20.end());
        p.load(0x0801, program);
        std::string error;
        const auto lines = detokenizeBasic(p, 0x0801, &error);
        CHECK(error.empty());
        CHECK_EQ(lines.size(), std::size_t{2});
        if (lines.size() == 2) {
            CHECK_EQ(lines[0].number, 10U);
            CHECK_EQ(lines[0].text, std::string("PRINT\"{CLR}HI\":REM {LT GREEN}"));
            CHECK_EQ(lines[1].number, 20U);
            CHECK_EQ(lines[1].text, std::string("SYS 4096"));
        }
        CHECK(!parseSysStub(p, 0x0801).has_value());
        CHECK(!findSysTarget(p, 0x0801).has_value());  // the first line has no SYS
        CHECK(findSysTarget(p, 0x0810) == std::optional<Address>(4096));
    }

    // broken programs are reported, not crashed on
    {
        Memory p;
        p.load(0x0801, std::vector<Byte>{0x03, 0x08, 0x0A, 0x00, 0x99});  // link points backwards
        std::string error;
        const auto lines = detokenizeBasic(p, 0x0801, &error);
        CHECK(lines.empty());
        CHECK(!error.empty());
        Memory q;
        q.load(0xFFF0, std::vector<Byte>{0xFF, 0xFF, 0x0A, 0x00, 0x99, 0x99, 0x99});
        (void)detokenizeBasic(q, 0xFFF0, &error);
        CHECK(!error.empty());
    }

    return finish("petscii");
}
