#include "m6510/expression.hpp"

#include <map>

#include "test_util.hpp"

using namespace m6510;

namespace {

std::map<std::string, std::int64_t> g_symbols = {{"start", 0x0801}, {"table", 0xC0FE}, {"D020", 7}, {"loop.x", 3}};

ExprResult ev(const std::string& s, std::int64_t pc = 0x1000, bool hex = false) {
    return ExpressionEvaluator(
               [](const std::string& n, std::int64_t& v) {
                   auto it = g_symbols.find(n);
                   if (it == g_symbols.end()) return false;
                   v = it->second;
                   return true;
               },
               pc, hex)
        .evaluate(s);
}

std::int64_t val(const std::string& s, std::int64_t pc = 0x1000, bool hex = false) {
    const ExprResult r = ev(s, pc, hex);
    if (!r.ok) std::cerr << "  expression '" << s << "': " << r.error << "\n";
    return r.value;
}

}  // namespace

int main() {
    // numbers
    CHECK_EQ(val("123"), 123);
    CHECK_EQ(val("$1F"), 0x1F);
    CHECK_EQ(val("$d020"), 0xD020);
    CHECK_EQ(val("0x1F"), 0x1F);
    CHECK_EQ(val("1Fh"), 0x1F);
    CHECK_EQ(val("0FFH"), 0xFF);
    CHECK_EQ(val("%1010"), 10);
    CHECK_EQ(val("0b1010"), 10);
    CHECK_EQ(val("'A'"), 65);
    CHECK_EQ(val("' '"), 32);

    // operators and precedence
    CHECK_EQ(val("1+2*3"), 7);
    CHECK_EQ(val("(1+2)*3"), 9);
    CHECK_EQ(val("[1+2]*3"), 9);
    CHECK_EQ(val("10-4-3"), 3);
    CHECK_EQ(val("7/2"), 3);
    CHECK_EQ(val("7%4"), 3);
    CHECK_EQ(val("1<<4|1"), 17);
    CHECK_EQ(val("$FF&$0F^$01"), 0x0E);
    CHECK_EQ(val("-1"), -1);
    CHECK_EQ(val("~0&$FF"), 0xFF);
    CHECK_EQ(val("$100>>4"), 0x10);

    // low / high byte
    CHECK_EQ(val("<$1234"), 0x34);
    CHECK_EQ(val(">$1234"), 0x12);
    CHECK_EQ(val(">table+1"), 0xC0);   // high byte of table+1 ($C0FF)
    CHECK_EQ(val("<table+1"), 0xFF);
    CHECK_EQ(val("<table+2"), 0x00);   // $C100
    CHECK_EQ(val(">table+2"), 0xC1);
    CHECK_EQ(val("LOW($1234)"), 0x34);
    CHECK_EQ(val("high($1234)"), 0x12);
    CHECK_EQ(val("(<start)+1"), 0x02);

    // current address
    CHECK_EQ(val("*"), 0x1000);
    CHECK_EQ(val("*+3"), 0x1003);
    CHECK_EQ(val("**2"), 0x2000);
    CHECK_EQ(val("*-2", 0xC000), 0xBFFE);

    // symbols
    CHECK_EQ(val("start+12"), 0x080D);
    CHECK_EQ(val("loop.x*2"), 6);
    {
        const ExprResult r = ev("nothere+1");
        CHECK(r.ok);
        CHECK(r.undefined);
        CHECK_EQ(r.undefinedName, std::string("nothere"));
    }

    // errors
    CHECK(!ev("").ok);
    CHECK(!ev("1+").ok);
    CHECK(!ev("(1+2").ok);
    CHECK(!ev("1/0").ok);
    CHECK(!ev("5%0").ok);
    CHECK(!ev("$").ok);
    CHECK(!ev("1 2").ok);
    CHECK(!ev("'AB'").ok);
    CHECK(!ev("12AB").ok);  // hex digits without $ in assembler mode
    CHECK(!ev("#5").ok);

    // monitor mode: hexadecimal by default
    CHECK_EQ(val("1000", 0, true), 0x1000);
    CHECK_EQ(val("FF", 0, true), 0xFF);
    CHECK_EQ(val("100.", 0, true), 100);
    CHECK_EQ(val("D020", 0, true), 7);  // a known symbol wins
    CHECK_EQ(val("C000+10", 0, true), 0xC010);
    CHECK(!ev("1F.", 0, true).ok);

    // deep nesting is rejected instead of overflowing the stack
    CHECK(!ev(std::string(5000, '(') + "1" + std::string(5000, ')')).ok);
    CHECK(!ev(std::string(5000, '-') + "1").ok);

    return finish("expression");
}
