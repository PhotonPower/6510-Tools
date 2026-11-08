#include <random>

#include "m6510/analysis.hpp"
#include "m6510/annotations.hpp"
#include "m6510/assembler.hpp"
#include "m6510/listing.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct Program {
    Memory mem;
    Address from = 0;
    std::uint32_t to = 0;
    std::map<std::string, std::int64_t> symbols;
    Address operator[](const std::string& n) const { return static_cast<Address>(symbols.at(n)); }
};

Program build(const std::string& source) {
    Program p;
    AssemblerOptions o;
    o.origin = 0xC000;
    const AssemblyResult r = assemble(source, o);
    if (!r.ok()) std::cerr << "assembly failed: " << r.errors[0].line << ": " << r.errors[0].message << "\n";
    for (const Segment& s : r.segments) p.mem.load(s.address, s.bytes);
    p.from = r.lowest();
    p.to = r.end();
    p.symbols = r.symbols;
    return p;
}

const JumpTable* find(const Analysis& an, JumpTable::Kind k) {
    for (const JumpTable& t : an.jumpTables())
        if (t.kind == k) return &t;
    return nullptr;
}

bool verifiedListing(const Program& p, const Analysis& an, const Annotations& ann, std::string* text = nullptr) {
    ListingOptions o;
    o.from = p.from;
    o.to = p.to;
    const ListingResult r = renderListing(p.mem, an, ann, o);
    if (text) *text = r.text;
    if (!r.verified) std::cerr << r.problem << "\n" << r.text;
    return r.verified;
}

const char* kCases = R"(
case0:  LDA #0
        RTS
case1:  LDA #1
        RTS
case2:  LDA #2
        RTS
)";

}  // namespace

int main() {
    // ---- RTS trick with split tables ---------------------------------------------------------------------
    {
        const Program p = build(std::string(R"(
main:   LDA hi,X
        PHA
        LDA lo,X
        PHA
        RTS
)") + kCases + R"(
lo:     .LO case0-1, case1-1, case2-1
hi:     .HI case0-1, case1-1, case2-1
after:  .BYTE $FF
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        const JumpTable* t = find(an, JumpTable::Kind::Rts);
        CHECK(t != nullptr);
        if (t) {
            CHECK(t->split);
            CHECK(t->minusOne);
            CHECK(t->followed);
            CHECK_EQ(t->jump, static_cast<Address>(p["main"] + 8));
            CHECK_EQ(t->table, p["lo"]);
            CHECK_EQ(t->hiTable, p["hi"]);
            CHECK_EQ(t->targets.size(), std::size_t{3});
            CHECK_EQ(t->targets[2], p["case2"]);
            CHECK_EQ(t->bytes, 6U);
        }
        CHECK(an.isCode(p["case1"]));
        CHECK(an.isTableData(p["lo"]) && an.isTableData(p["hi"] + 2));
        CHECK(!an.isTableData(p["after"]));
        int viaTable = 0;
        for (const RoutineInfo& r : an.routines()) viaTable += r.viaTable;
        CHECK_EQ(viaTable, 3);
        std::string text;
        CHECK(verifiedListing(p, an, Annotations{}, &text));
        CHECK(has(text, ".LO sub_" + hexDigits(p["case0"], 4) + "-1,"));
        CHECK(has(text, ".HI sub_" + hexDigits(p["case0"], 4) + "-1,"));
    }

    // ---- RTS trick with a word table (LDA tab+1,X / PHA / LDA tab,X / PHA / RTS) -------------------------
    {
        const Program p = build(std::string(R"(
main:   TXA
        ASL A
        TAX
        LDA tab+1,X
        PHA
        LDA tab,X
        PHA
        RTS
)") + kCases + R"(
tab:    .WORD case0-1, case1-1, case2-1
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        const JumpTable* t = find(an, JumpTable::Kind::Rts);
        CHECK(t != nullptr && !t->split && t->targets.size() == 3 && t->slots[1] == p["tab"] + 2);
        std::string text;
        CHECK(verifiedListing(p, an, Annotations{}, &text));
        CHECK(has(text, ".WORD sub_"));
    }

    // ---- JMP (vector) through split tables, the stores in either order --------------------------------------
    for (const bool hiFirst : {false, true}) {
        const std::string setup = hiFirst ? "LDA hi,Y\n STA vec+1\n LDA lo,Y\n STA vec\n" : "LDA lo,Y\n STA vec\n LDA hi,Y\n STA vec+1\n";
        const Program p = build("vec = $FB\nmain: " + setup + " JMP (vec)\n" + kCases +
                                "lo: .LO case0, case1, case2\nhi: .HI case0, case1, case2\n");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        const JumpTable* t = find(an, JumpTable::Kind::Lohi);
        CHECK(t != nullptr && t->split && !t->minusOne && t->targets.size() == 3);
        if (t) CHECK_EQ(t->targets[1], p["case1"]);
        CHECK(an.isCode(p["case2"]));
        CHECK(verifiedListing(p, an, Annotations{}));
    }

    // ---- JMP (vector) with a fixed vector inside the program; a vector the code writes is not followed ---
    {
        const Program p = build(std::string(R"(
main:   JMP (vec)
)") + kCases + R"(
vec:    .WORD case1
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        const JumpTable* t = find(an, JumpTable::Kind::Vector);
        CHECK(t != nullptr && t->targets.size() == 1 && t->targets[0] == p["case1"]);
        CHECK(an.isCode(p["case1"]));
        CHECK(!an.isCode(p["case0"]));
        std::string text;
        CHECK(verifiedListing(p, an, Annotations{}, &text));
        CHECK(has(text, ".WORD sub_" + hexDigits(p["case1"], 4)));
    }
    {
        const Program p = build(std::string(R"(
main:   LDA #<case2
        STA vec
        JMP (vec)
)") + kCases + R"(
vec:    .WORD case1
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        CHECK(find(an, JumpTable::Kind::Vector) == nullptr);
        CHECK(!an.isCode(p["case1"]));
        CHECK(verifiedListing(p, an, Annotations{}));
    }

    // ---- counterexamples ---------------------------------------------------------------------------------
    {
        // an RTS without the pattern, a JMP (zp) whose vector nobody sets up, a table that points outside
        const Program p = build(std::string(R"(
main:   LDA lo,X
        PHA
        PHA
        RTS
m2:     JMP ($FB)
m3:     LDA hi,X
        PHA
        LDA lo,X
        PHA
        RTS
)") + kCases + R"(
lo:     .BYTE $00
hi:     .BYTE $10
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.addEntry(p["m2"]);
        an.addEntry(p["m3"]);
        an.analyzeFlow();
        CHECK(an.jumpTables().empty());  // m3: the target $1001 is outside the range
        CHECK(!an.isCode(p["case0"]));
        CHECK(verifiedListing(p, an, Annotations{}));
    }

    // ---- pointer tables: a suggestion from the scan, followed after an annotation ---------------------------
    {
        const Program p = build(std::string(R"(
main:   JSR case0
        RTS
)") + kCases + R"(
tab:    .WORD case0, case0, case0
lone:   .WORD case1, case2
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        const JumpTable* t = find(an, JumpTable::Kind::Pointers);
        CHECK(t != nullptr && !t->followed && t->table == p["tab"] && t->targets.size() == 3);
        CHECK(!an.isCode(p["case1"]));
        Annotations ann;
        DataRegion r;
        r.address = p["lone"];
        r.length = 4;
        r.kind = DataKind::Pointers;
        CHECK(!ann.setData(r));
        Analysis an2(p.mem, p.from, p.to);
        an2.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
        an2.addEntry(p["main"]);
        an2.addPointerTable(r.address, r.length, false);
        an2.analyzeFlow();
        CHECK(an2.isCode(p["case1"]) && an2.isCode(p["case2"]));
        std::string text;
        CHECK(verifiedListing(p, an2, ann, &text));
        CHECK(has(text, ".WORD sub_" + hexDigits(p["case1"], 4) + ",sub_" + hexDigits(p["case2"], 4)));
        // the same as split tables with the RTS offset, annotated
        const Program q = build(std::string("main: RTS\n") + kCases + "lo: .LO case0-1, case2-1\nhi: .HI case0-1, case2-1\n");
        Annotations a2;
        DataRegion l;
        l.address = q["lo"];
        l.partner = q["hi"];
        l.length = 2;
        l.kind = DataKind::Lohi;
        l.minusOne = true;
        CHECK(!a2.setData(l));
        Analysis an3(q.mem, q.from, q.to);
        an3.setDataBarrier([&a2](Address a) { return a2.dataCovering(a) != nullptr; });
        an3.addEntry(q["main"]);
        an3.addLohiTable(l.address, l.partner, l.length, true);
        an3.analyzeFlow();
        CHECK(an3.isCode(q["case2"]) && !an3.isCode(q["case1"]));
        CHECK(verifiedListing(q, an3, a2, &text));
        CHECK(has(text, ".HI sub_" + hexDigits(q["case0"], 4) + "-1,sub_" + hexDigits(q["case2"], 4) + "-1"));
    }

    // ---- random dispatchers: n cases, every case found, the listing verified ------------------------------------
    {
        std::mt19937 gen(1234);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        for (int iter = 0; iter < 60; ++iter) {
            const unsigned n = 1 + rng() % 12;
            const int style = static_cast<int>(rng() % 3);  // 0 RTS split, 1 RTS words, 2 JMP (vector) split
            std::string src = "vec = $FB\nmain: ";
            if (style == 0) src += "LDA hi,X\n PHA\n LDA lo,X\n PHA\n RTS\n";
            if (style == 1) src += "LDA tab+1,X\n PHA\n LDA tab,X\n PHA\n RTS\n";
            if (style == 2) src += "LDA lo,X\n STA vec\n LDA hi,X\n STA vec+1\n JMP (vec)\n";
            std::string list;
            for (unsigned i = 0; i < n; ++i) {
                src += "c" + std::to_string(i) + ": LDA #" + std::to_string(i) + "\n";
                for (unsigned k = 0; k < rng() % 4; ++k) src += " NOP\n";
                src += " RTS\n";
                list += (i ? ", c" : "c") + std::to_string(i) + (style < 2 ? "-1" : "");
            }
            if (style == 1) src += "tab: .WORD " + list + "\n";
            else src += "lo: .LO " + list + "\nhi: .HI " + list + "\n";
            src += ".BYTE $FF\n";
            const Program p = build(src);
            Analysis an(p.mem, p.from, p.to);
            an.addEntry(p["main"]);
            an.analyzeFlow();
            CHECK_EQ(an.jumpTables().size(), std::size_t{1});
            if (an.jumpTables().size() == 1) CHECK_EQ(an.jumpTables()[0].targets.size(), std::size_t{n});
            for (unsigned i = 0; i < n; ++i) CHECK(an.isCode(p["c" + std::to_string(i)]));
            CHECK(verifiedListing(p, an, Annotations{}));
        }
    }

    return finish("jumptable");
}
