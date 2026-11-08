#include <cstdlib>
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
};

Program build(const std::string& source) {
    Program p;
    AssemblerOptions o;
    o.level = CpuLevel::All;
    const AssemblyResult r = assemble(source, o);
    if (!r.ok()) std::cerr << "assembly failed: " << r.errors[0].line << ": " << r.errors[0].message << "\n";
    for (const Segment& s : r.segments) p.mem.load(s.address, s.bytes);
    p.from = r.lowest();
    p.to = r.end();
    p.symbols = r.symbols;
    return p;
}

Address sym(const Program& p, const std::string& name) { return static_cast<Address>(p.symbols.at(name)); }

ListingResult listing(const Program& p, const Analysis& an, const Annotations& ann, bool addresses = false) {
    ListingOptions o;
    o.from = p.from;
    o.to = p.to;
    o.addresses = addresses;
    return renderListing(p.mem, an, ann, o);
}

}  // namespace

int main() {
    // ---- annotations: names, rules, lookups, JSON -------------------------------------------------------
    {
        Annotations a;
        CHECK(!a.setLabel(0xC000, "start"));
        CHECK(a.setLabel(0xC001, "start").has_value());   // duplicate (case-insensitive too)
        CHECK(a.setLabel(0xC001, "START").has_value());
        CHECK(a.setLabel(0xC001, "LDA").has_value());     // mnemonic
        CHECK(a.setLabel(0xC001, "lax").has_value());     // undocumented mnemonic
        CHECK(a.setLabel(0xC001, "X").has_value());       // register
        CHECK(a.setLabel(0xC001, "CHROUT").has_value());  // KERNAL entry
        CHECK(a.setLabel(0xC001, "EXTCOL").has_value());  // chip register
        CHECK(a.setLabel(0xC001, "sub_C001").has_value());// placeholder
        CHECK(a.setLabel(0xC001, "loc_beef").has_value());
        CHECK(!a.setLabel(0xC001, "sub_C0011"));          // not a placeholder (5 digits)
        CHECK(a.setLabel(0xC002, "1abc").has_value());
        CHECK(a.setLabel(0xC002, "a-b").has_value());
        CHECK(a.setLabel(0xC002, ".local").has_value());
        CHECK(a.setLabel(0xC002, "").has_value());
        CHECK(a.setLabel(0xC002, std::string(65, 'x')).has_value());
        CHECK(!a.setLabel(0xC000, "begin"));              // renaming frees the old name
        CHECK(!a.setLabel(0xC003, "start"));
        CHECK_EQ(*a.nameAt(0xC000), std::string("begin"));
        CHECK_EQ(*a.addressOf("START"), Address{0xC003});
        RoutineDoc d;
        d.summary = "does it";
        d.confidence = "maybe";
        CHECK(a.setRoutine(0xC000, d).has_value());
        d.confidence = "high";
        CHECK(!a.setRoutine(0xC000, d));
        CHECK_EQ(a.routineAt(0xC000)->name, std::string("begin"));
        CHECK(!a.setLabel(0xC000, "renamed"));
        CHECK_EQ(a.routineAt(0xC000)->name, std::string("renamed"));  // kept consistent
        VariableDoc v;
        v.name = "ptr";
        v.address = 0xFB;
        v.size = 2;
        CHECK(!a.setVariable(v));
        CHECK_EQ(*a.expressionFor(0xFC), std::string("ptr+1"));
        v.size = 0;
        CHECK(a.setVariable(v).has_value());
        DataRegion r;
        r.address = 0xD000 - 0x1000;
        r.length = 16;
        r.kind = DataKind::Text;
        r.name = "msg";
        CHECK(!a.setData(r));
        CHECK_EQ(*a.expressionFor(0xC00F), std::string("msg+$F"));
        CHECK_EQ(*a.expressionFor(0xC005), std::string("msg+5"));
        DataRegion o = r;
        o.address = 0xC008;
        o.name.clear();
        CHECK(a.setData(o).has_value());  // overlaps
        DataRegion w;
        w.address = 0xC100;
        w.length = 3;
        w.kind = DataKind::Words;
        CHECK(a.setData(w).has_value());  // odd length
        DataRegion l;
        l.address = 0xC200;
        l.partner = 0xC202;
        l.length = 3;
        l.kind = DataKind::Lohi;
        CHECK(a.setData(l).has_value());  // the tables overlap
        l.partner = 0xC203;
        l.minusOne = true;
        CHECK(!a.setData(l));
        CHECK(a.dataAt(0xC203) != nullptr);  // the partner side
        CHECK(a.dataCovering(0xC205) != nullptr);
        CHECK(a.dataCovering(0xC206) == nullptr);
        a.setComment(0xC000, "hello");
        a.setBlockComment(0xC000, "block");
        a.setConstant(0xC010, true);
        a.setTitle("test");
        // JSON round trip
        const Json j = a.toJson();
        const Annotations b = Annotations::fromJson(Json::parse(j.dump()));
        CHECK_EQ(b.toJson().dump(), j.dump());
        CHECK(b.isConstant(0xC010));
        CHECK(b.dataAt(0xC203)->minusOne);
        // rule violations in a file are rejected
        bool threw = false;
        try {
            (void)Annotations::fromJson(Json::parse(R"({"labels":[{"addr":"0xC000","name":"LDA"}]})"));
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            (void)Annotations::fromJson(Json::parse(R"({"data":[{"addr":"0xC000","length":5,"kind":"floats"}]})"));
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            (void)Annotations::fromJson(Json::parse(R"({"labels":7})"));
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
        // removing
        CHECK(a.removeLabel(0xC000));
        CHECK(!a.routineAt(0xC000));
        CHECK(a.removeComment(0xC000));
        CHECK(!a.removeComment(0xC000));
    }

    // ---- analysis: flow, routines, KERNAL calls, I/O, zero page, port, self-modifying code -------------
    {
        const Program p = build(R"(
CHROUT = $FFD2
        * = $C000
main:   JSR sub1
        JSR sub2
        LDA #$35
        STA $01
        JMP tail
sub1:   LDA ($FB),Y
        STA $D020
        LDA $D012
        JSR CHROUT
        RTS
sub2:   JSR sub1
        INC patch+1
patch:  LDX #0
        RTS
tail:   BEQ tail2
        NOP
tail2:  RTS
data:   .BYTE 1, 2, 3
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(sym(p, "main"));
        an.analyzeFlow();
        CHECK(an.isCode(sym(p, "sub2")));
        CHECK(!an.isCode(sym(p, "data")));
        CHECK(an.at(sym(p, "patch")) != nullptr);
        CHECK(an.isCode(sym(p, "patch") + 1) && !an.at(sym(p, "patch") + 1));
        CHECK_EQ(*an.instructionStart(sym(p, "patch") + 1), sym(p, "patch"));
        CHECK(an.selfModified().count(sym(p, "patch")) == 1);
        const auto routines = an.routines();
        std::map<Address, RoutineInfo> by;
        for (const RoutineInfo& r : routines) by[r.entry] = r;
        CHECK_EQ(routines.size(), std::size_t{3});  // main, sub1, sub2 (tail is reached by JMP only: part of main)
        const RoutineInfo& s1 = by.at(sym(p, "sub1"));
        CHECK_EQ(s1.depth, 0);
        CHECK(s1.ioWrites.count(0xD020) == 1);
        CHECK(s1.ioReads.count(0xD012) == 1);
        CHECK(s1.zeroPage.count(0xFB) == 1 && s1.zeroPage.count(0xFC) == 1);
        CHECK_EQ(s1.kernalCalls.size(), std::size_t{1});
        CHECK_EQ(s1.callers.size(), std::size_t{2});
        const RoutineInfo& m = by.at(sym(p, "main"));
        CHECK_EQ(m.depth, 2);
        CHECK_EQ(m.portWrites.size(), std::size_t{1});
        CHECK_EQ(m.portWrites[0].second, 0x35);
        CHECK_EQ(m.blocks.size(), std::size_t{2});  // its own code and the tail behind the subroutines
        CHECK_EQ(m.instructions, 8U);
        // the listing: verified, with names, equates and the self-modifying label
        Annotations ann;
        ListingResult r = listing(p, an, ann);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, "CHROUT          = $FFD2"));
        CHECK(has(r.text, "EXTCOL          = $D020"));
        CHECK(has(r.text, "INC loc_" + hexDigits(sym(p, "patch"), 4) + "+1"));
        CHECK(has(r.text, "operand changed by the code at"));
        CHECK_EQ(r.instructions, 17U);
        // names, comments, a routine header
        CHECK(!ann.setLabel(sym(p, "sub1"), "show"));
        RoutineDoc d;
        d.summary = "shows a character";
        d.inputs = "Y = index";
        CHECK(!ann.setRoutine(sym(p, "sub1"), d));
        ann.setComment(sym(p, "main"), "start here");
        ann.setBlockComment(sym(p, "tail"), "the end\nof everything");
        VariableDoc v;
        v.name = "ptr";
        v.address = 0xFB;
        v.size = 2;
        CHECK(!ann.setVariable(v));
        DataRegion dr;
        dr.address = sym(p, "data");
        dr.length = 3;
        dr.name = "numbers";
        dr.comment = "three numbers";
        CHECK(!ann.setData(dr));
        r = listing(p, an, ann, true);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, "; show: shows a character"));
        CHECK(has(r.text, ";   in:       Y = index"));
        CHECK(has(r.text, "JSR show"));
        CHECK(has(r.text, "LDA (ptr),Y"));
        CHECK(has(r.text, "ptr             = $00FB"));
        CHECK(has(r.text, "; $C000: start here"));
        CHECK(has(r.text, "; the end\n; of everything\n"));
        CHECK(has(r.text, "numbers:\n        .BYTE $01,$02,$03"));
        CHECK(has(r.text, ": three numbers"));
        CHECK_EQ(r.userComments, 1U);
        // a part of the range: names outside it are the same as in the whole listing (and EQU lines keep it assemblable)
        ListingOptions o;
        o.from = sym(p, "sub2");
        o.to = sym(p, "tail");
        r = renderListing(p.mem, an, ann, o);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, "JSR show"));
        CHECK(!has(r.text, "main:"));
        o.equates = false;
        o.verify = false;
        r = renderListing(p.mem, an, ann, o);
        CHECK(!has(r.text, ".CPU"));
        CHECK(has(r.text, "JSR show"));
        o.from = sym(p, "tail");
        o.to = p.to;
        r = renderListing(p.mem, an, ann, o);
        CHECK(has(r.text, "BEQ loc_"));                        // inside the excerpt
        o.from = sym(p, "main");
        o.to = sym(p, "sub1");
        r = renderListing(p.mem, an, ann, o);
        CHECK(has(r.text, "JMP loc_" + hexDigits(sym(p, "tail"), 4)));  // outside: the same placeholder name
        o.from = p.from;
        o.to = p.to;
        o.maxChars = 200;
        r = renderListing(p.mem, an, ann, o);
        CHECK(r.truncated);
        CHECK(!r.verified);
        CHECK(r.text.size() < 260);
    }

    // ---- comments on data, variables split from zero runs, variable comments on EQU lines -------------------
    {
        const Program p = build(R"(
        * = $C000
main:   INC counter
        LDA table+3
        STA $FB
        RTS
counter: .BYTE 0
        .FILL 30
table:  .FILL 20
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(sym(p, "main"));
        an.analyzeFlow();
        Annotations ann;
        VariableDoc v;
        v.name = "zp_out";
        v.address = 0xFB;
        v.comment = "result for the caller";
        CHECK(!ann.setVariable(v));
        ann.setComment(static_cast<Address>(sym(p, "table") + 5), "the sixth entry");
        ListingResult r = listing(p, an, ann, true);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, "zp_out          = $00FB                 ; result for the caller"));
        CHECK(has(r.text, "data_" + hexDigits(sym(p, "counter"), 4) + ":\n        .BYTE $00"));  // the counter is not swallowed by .FILL
        CHECK(has(r.text, ": the sixth entry"));
        v.name = "count";
        v.address = sym(p, "counter");
        v.comment = "how often main ran";
        CHECK(!ann.setVariable(v));
        r = listing(p, an, ann, true);
        CHECK(r.verified);
        CHECK(has(r.text, "count:\n        .BYTE $00                       ; $" + hexDigits(sym(p, "counter"), 4) + ": how often main ran"));
    }

    // ---- a range that covers the I/O and KERNAL areas: chips, KERNAL entries and colour RAM keep their names --------------
    {
        Memory mem;
        AssemblerOptions o;
        const AssemblyResult ar = assemble(" * = $C000\nmain: INC $D020\n STA $D850,X\n JSR $FFD2\n JSR $DD03\n RTS\n", o);
        for (const Segment& seg : ar.segments) mem.load(seg.address, seg.bytes);
        mem.write8(0xDD03, 0x60);  // RAM under the CIA (the program calls the register, which holds RTS)
        Annotations ann;
        DataRegion io;
        io.address = 0xD000;
        io.length = 0x3000;
        io.name = "ram_d000";
        CHECK(!ann.setData(io));
        Analysis an(mem, 0xC000, 0x10000);
        an.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
        an.addEntry(0xC000);
        an.analyzeFlow();
        CHECK(!an.isCode(0xDD03));
        ListingOptions lo;
        lo.from = 0xC000;
        lo.to = 0x10000;
        const ListingResult r = renderListing(mem, an, ann, lo);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n";
        CHECK(has(r.text, "INC EXTCOL"));
        CHECK(has(r.text, "STA COLRAM+$50,X"));
        CHECK(has(r.text, "JSR CHROUT"));
        CHECK(has(r.text, "JSR CI2DDRB") || has(r.text, "JSR C2DDRB"));
        CHECK(!has(r.text, "data_D020"));
        // a name the user gives wins (the program banks the ROM out and keeps a table there), in an excerpt as well
        VariableDoc v;
        v.address = 0xD850;
        v.name = "row_colours";
        v.size = 0x40;
        CHECK(!ann.setVariable(v));
        lo.to = 0xC00C;
        const ListingResult e = renderListing(mem, an, ann, lo);
        CHECK(e.verified);
        CHECK(has(e.text, "STA row_colours,X"));
        CHECK(has(e.text, "INC EXTCOL"));
        // ... and inside a large plain region it gets a line and a label of its own
        lo.to = 0x10000;
        const ListingResult f = renderListing(mem, an, ann, lo);
        CHECK(f.verified);
        CHECK(has(f.text, "row_colours:"));
        CHECK(has(f.text, "STA row_colours,X"));
    }

    // ---- pointer constants in immediate pairs, interrupt handlers, linear analysis ----------------------
    {
        const Program p = build(R"(
        * = $1000
main:   LDA #<text
        LDY #>text
        JSR $AB1E
        LDX #<buffer
        LDY #>buffer
        JSR $FFBD
        LDA #<irq
        STA $0314
        LDA #>irq
        STA $0315
        LDA #<$1234      ; outside the range: stays a number
        LDY #>$1234
        RTS
irq:    INC $D019
        JMP $EA31
text:   .BYTE "HI", 0
buffer: .BYTE 0, 0
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(sym(p, "main"));
        an.analyzeFlow();
        CHECK(an.isCode(sym(p, "irq")));
        CHECK_EQ(an.vectorEntries().at(sym(p, "irq")), Address{0x0314});
        CHECK_EQ(an.pointerImmediates().size(), std::size_t{6});
        CHECK(an.pointerImmediates().at(sym(p, "main")).second);  // low byte
        Annotations ann;
        ann.setConstant(static_cast<Address>(sym(p, "main") + 7), true);  // LDX #<buffer is a plain number
        const ListingResult r = listing(p, an, ann);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, "LDA #<data_" + hexDigits(sym(p, "text"), 4)));
        CHECK(has(r.text, "LDY #>data_" + hexDigits(sym(p, "text"), 4)));
        CHECK(has(r.text, "LDX #$" + hexDigits(sym(p, "buffer") & 0xFF, 2)));
        CHECK(has(r.text, "LDA #<sub_" + hexDigits(sym(p, "irq"), 4)));
        CHECK(has(r.text, "LDA #$34"));
        // linear analysis of bytes nobody jumps to
        Analysis lin(p.mem, p.from, p.to);
        lin.analyzeLinear(p.from, sym(p, "text"));
        CHECK(lin.isCode(sym(p, "irq")));
        CHECK(lin.at(sym(p, "main")) != nullptr);
    }

    // ---- BASIC starter, non-preferred opcodes, unstable opcodes, zero runs, code at the end -------------
    {
        const Program p = build(R"(
        * = $0801
        .BASIC 10, start
start:  NOP $12             ; undocumented
        LAX ($10),Y
        ANE #$00            ; unstable
        .BYTE $EB, $05      ; SBC #5 with the non-preferred opcode
        .BYTE $1A           ; NOP (non-preferred)
        LDA.W $0012         ; absolute with a zero-page value
        RTS
        .FILL 40
        .BYTE 1
)");
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(sym(p, "start"));
        an.analyzeFlow();
        Annotations ann;
        const ListingResult r = listing(p, an, ann);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, ".BASIC 10,sub_080D"));
        CHECK(has(r.text, "USBC #$05"));
        CHECK(has(r.text, ".BYTE $1A"));
        CHECK(has(r.text, "(alternative opcode)"));
        CHECK(has(r.text, "LDA.W $0012"));
        CHECK(has(r.text, ".FILL 40"));
        CHECK(has(r.text, "ANE #$00"));
    }

    // ---- random programs with random annotations: the listing is verified every time -------------------
    {
        std::mt19937 gen(6510);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        int verified = 0, runs = 0;
        for (int iter = 0; iter < 2000; ++iter) {
            Memory mem;
            const Address from = static_cast<Address>(0x0800 + (rng() % 0x80) * 0x100);
            const unsigned size = 64 + rng() % 400;
            for (unsigned i = 0; i < size; ++i) mem.write8(static_cast<Address>(from + i), static_cast<Byte>(rng()));
            // sprinkle some structure: zero runs, text, RTS
            if (rng() % 2) for (unsigned i = 0; i < 20; ++i) mem.write8(static_cast<Address>(from + size / 2 + i), 0);
            if (rng() % 2) for (unsigned i = 0; i < 6; ++i) mem.write8(static_cast<Address>(from + size / 3 + i), static_cast<Byte>('A' + i));
            const std::uint32_t to = from + size;
            Analysis an(mem, from, to);
            for (int e = 0; e < 1 + static_cast<int>(rng() % 4); ++e) an.addEntry(static_cast<Address>(from + rng() % size));
            if (rng() % 3 == 0) an.analyzeLinear(from, from + size / 4);
            an.analyzeFlow();
            Annotations ann;
            // random names on instruction starts and data, random comments and regions in non-code gaps
            int named = 0;
            for (const auto& [a, in] : an.code()) {
                if (rng() % 4 == 0) (void)ann.setLabel(a, "n" + std::to_string(named++));
                if (rng() % 5 == 0) ann.setComment(a, "comment " + std::to_string(a));
                if (rng() % 9 == 0) ann.setBlockComment(a, "block\ncomment");
                if (rng() % 7 == 0) ann.setConstant(a, true);
            }
            for (int k = 0; k < 6; ++k) {
                DataRegion r;
                r.address = static_cast<Address>(from + rng() % size);
                r.length = 1 + rng() % 24;
                const DataKind kinds[] = {DataKind::Bytes, DataKind::Words, DataKind::Text, DataKind::Screen, DataKind::Pointers,
                                          DataKind::RtsPointers, DataKind::Lohi, DataKind::Sprites, DataKind::Charset};
                r.kind = kinds[rng() % 9];
                if (r.kind == DataKind::Words || r.kind == DataKind::Pointers || r.kind == DataKind::RtsPointers) r.length &= ~1U;
                if (r.length == 0) r.length = 2;
                r.partner = static_cast<Address>(r.address + r.length + rng() % 8);
                r.minusOne = rng() % 2;
                if (rng() % 2) r.name = "d" + std::to_string(k);
                if (rng() % 2) r.comment = "data " + std::to_string(k);
                bool free = r.address + r.length <= to && (r.kind != DataKind::Lohi || r.partner + r.length <= to);
                for (unsigned i = 0; free && i < r.length; ++i) {
                    free = !an.isCode(static_cast<Address>(r.address + i)) && !an.isTableData(static_cast<Address>(r.address + i));
                    if (r.kind == DataKind::Lohi)
                        free = free && !an.isCode(static_cast<Address>(r.partner + i)) && !an.isTableData(static_cast<Address>(r.partner + i));
                }
                if (free) (void)ann.setData(r);
            }
            VariableDoc v;
            v.name = "zp";
            v.address = static_cast<Address>(rng() % 0x100);
            v.size = 1 + rng() % 3;
            if (v.address + v.size <= 0x100) (void)ann.setVariable(v);
            ListingOptions o;
            o.from = from;
            o.to = to;
            o.addresses = rng() % 2;
            o.c64Names = rng() % 2;
            const ListingResult r = renderListing(mem, an, ann, o);
            ++runs;
            if (r.verified) ++verified;
            else if (runs - verified <= 3) {
                std::cerr << "not verified (iteration " << iter << "): " << r.problem << "\n";
                if (std::getenv("M6510_SHOW_LISTING")) std::cerr << r.text;
            }
        }
        CHECK_EQ(verified, runs);
    }

    return finish("reverse");
}
