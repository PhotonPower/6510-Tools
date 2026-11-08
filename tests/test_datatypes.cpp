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

std::vector<DataGuess> guesses(const Program& p, Address entry, Analysis** keep = nullptr) {
    auto* an = new Analysis(p.mem, p.from, p.to);
    an->addEntry(entry);
    an->analyzeFlow();
    std::vector<DataGuess> g = an->guessData();
    if (keep) *keep = an;
    else delete an;
    return g;
}

const DataGuess* at(const std::vector<DataGuess>& g, Address a) {
    for (const DataGuess& x : g)
        if (x.start == a) return &x;
    return nullptr;
}

DataKind toDataKind(DataGuess::Kind k) {
    switch (k) {
        case DataGuess::Kind::Text: return DataKind::Text;
        case DataGuess::Kind::Screen: return DataKind::Screen;
        case DataGuess::Kind::Words: return DataKind::Words;
        case DataGuess::Kind::Pointers: return DataKind::Pointers;
        case DataGuess::Kind::Sprites: return DataKind::Sprites;
        case DataGuess::Kind::Charset: return DataKind::Charset;
        default: return DataKind::Bytes;
    }
}

}  // namespace

int main() {
    // ---- text, screen codes, zeros, word tables ------------------------------------------------------------
    {
        const Program p = build(R"(
main:   LDA #<msg
        LDY #>msg
        JSR $AB1E
        LDX #0
copy:   LDA scr,X
        STA $0400,X
        LDA tab,X
        LDY tab+1,X
        INX
        CPX #5
        BNE copy
        RTS
msg:    .BYTE "HELLO WORLD", 13, 0
short:  .BYTE "OK", 0
scr:    .SCREEN "screen text"
nums:   .BYTE $FF, $FE
zeros:  .FILL 20
tab:    .WORD $1234, $5678, $9ABC
noise:  .BYTE $FF, $80, $FF, $81, $FF, $82
)");
        Analysis* an = nullptr;
        const std::vector<DataGuess> g = guesses(p, p["main"], &an);
        const DataGuess* m = at(g, p["msg"]);
        CHECK(m && m->kind == DataGuess::Kind::Text && m->length == 13);
        if (m) CHECK_EQ(m->preview, std::string("HELLO WORLD{RETURN}"));
        const DataGuess* s = at(g, p["scr"]);
        CHECK(s && s->kind == DataGuess::Kind::Screen && s->length == 11);  // stops at the referenced 'nums'? no: at $FF
        if (s) CHECK_EQ(s->preview, std::string("SCREEN TEXT"));
        const DataGuess* z = at(g, p["zeros"]);
        CHECK(z && z->kind == DataGuess::Kind::Zeros && z->length == 20);
        const DataGuess* w = at(g, p["tab"]);
        CHECK(w && w->kind == DataGuess::Kind::Words && w->length >= 6);  // unreferenced bytes behind it may belong to it
        CHECK(at(g, p["short"]) == nullptr);  // not referenced and too short
        CHECK(at(g, p["noise"]) == nullptr);
        CHECK(at(g, p["nums"]) == nullptr);
        // no guess overlaps another or the code
        for (std::size_t i = 0; i < g.size(); ++i) {
            for (unsigned k = 0; k < g[i].length; ++k) CHECK(!an->isCode(static_cast<Address>(g[i].start + k)));
            if (i > 0) CHECK(g[i - 1].start + g[i - 1].length <= g[i].start);
        }
        // applied as annotations, the listing shows the types and is verified
        Annotations ann;
        for (const DataGuess& x : g) {
            if (x.kind == DataGuess::Kind::Zeros) continue;
            DataRegion r;
            r.address = x.start;
            r.length = x.length;
            r.kind = toDataKind(x.kind);
            CHECK(!ann.setData(r));
        }
        ListingOptions o;
        o.from = p.from;
        o.to = p.to;
        const ListingResult r = renderListing(p.mem, *an, ann, o);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.problem << "\n" << r.text;
        CHECK(has(r.text, ".BYTE \"HELLO WORLD\",$0D,$00"));
        CHECK(has(r.text, ".SCREEN \"screen text\""));
        CHECK(has(r.text, ".WORD $1234,$5678,$9ABC"));
        CHECK(has(r.text, ".FILL 20"));
        delete an;
    }

    // ---- sprites and character sets from the code that uses them ---------------------------------------------
    {
        const Program p = build(R"(
        * = $2000
main:   LDA #sprite/64
        STA $07F8
        LDA #$1C           ; character set at $3000
        STA $D018
        LDA #$14           ; the ROM character set: nothing to guess
        STA $D018
        RTS
        .ALIGN 64
sprite: .FILL 63, $AA
        .BYTE 0
        .FILL $3000-*, $EA
charset:
        .FILL 2048, $55
)");
        const std::vector<DataGuess> g = guesses(p, p["main"]);
        const DataGuess* sp = at(g, p["sprite"]);
        CHECK(sp && sp->kind == DataGuess::Kind::Sprites && sp->length == 64);
        const DataGuess* cs = at(g, 0x3000);
        CHECK(cs && cs->kind == DataGuess::Kind::Charset && cs->length == 2048);
        int charsets = 0;
        for (const DataGuess& x : g) charsets += x.kind == DataGuess::Kind::Charset;
        CHECK_EQ(charsets, 1);
        Analysis an(p.mem, p.from, p.to);
        an.addEntry(p["main"]);
        an.analyzeFlow();
        Annotations ann;
        DataRegion r;
        r.address = p["sprite"];
        r.length = 64;
        r.kind = DataKind::Sprites;
        CHECK(!ann.setData(r));
        r.address = 0x3000;
        r.length = 2048;
        r.kind = DataKind::Charset;
        CHECK(!ann.setData(r));
        ListingOptions o;
        o.from = p.from;
        o.to = p.to;
        const ListingResult lr = renderListing(p.mem, an, ann, o);
        CHECK(lr.verified);
        CHECK(has(lr.text, ".BYTE $AA,$AA,$AA               ; #.#.#.#.#.#.#.#.#.#.#.#."));
        CHECK(has(lr.text, "; $FF"));  // the last character of the set
    }

    // ---- a BASIC program, and counterexamples ---------------------------------------------------------------
    {
        Program p = build(R"(
        * = $0801
        .BASIC 10, start
start:  RTS
        .BYTE 1, 2, 3, 0
)");
        const std::vector<DataGuess> g = guesses(p, 0x080D);
        const DataGuess* b = at(g, 0x0801);
        CHECK(b && b->kind == DataGuess::Kind::Basic && b->length == 12);
        if (b) CHECK_EQ(b->preview, std::string("10 SYS2061"));
        // a sprite pointer to a block outside the range, a charset in the ROM area: nothing
        const Program q = build(R"(
        * = $C000
        LDA #13
        STA $07F8
        RTS
)");
        CHECK(guesses(q, 0xC000).empty());
    }

    // ---- random images: guesses never overlap code or each other, the listing with them is verified ----------
    {
        std::mt19937 gen(64);
        auto rng = [&gen]() { return static_cast<unsigned>(gen()); };
        for (int iter = 0; iter < 200; ++iter) {
            Memory mem;
            const Address from = 0x4000;
            const unsigned size = 100 + rng() % 600;
            for (unsigned i = 0; i < size; ++i) {
                const unsigned kind = rng() % 10;
                Byte b = static_cast<Byte>(rng());
                if (kind < 3) b = static_cast<Byte>('A' + rng() % 26);
                else if (kind == 3) b = 0;
                else if (kind == 4) b = static_cast<Byte>(1 + rng() % 26);
                mem.write8(static_cast<Address>(from + i), b);
            }
            Analysis an(mem, from, from + size);
            for (int e = 0; e < 3; ++e) an.addEntry(static_cast<Address>(from + rng() % size));
            an.analyzeFlow();
            const std::vector<DataGuess> g = an.guessData();
            Annotations ann;
            for (std::size_t i = 0; i < g.size(); ++i) {
                CHECK(g[i].length > 0);
                for (unsigned k = 0; k < g[i].length; ++k) CHECK(!an.isCode(static_cast<Address>(g[i].start + k)));
                if (i > 0) CHECK(g[i - 1].start + g[i - 1].length <= g[i].start);
                if (g[i].kind == DataGuess::Kind::Zeros || g[i].kind == DataGuess::Kind::Basic) continue;
                DataRegion r;
                r.address = g[i].start;
                r.length = g[i].length;
                r.kind = toDataKind(g[i].kind);
                if ((r.kind == DataKind::Words || r.kind == DataKind::Pointers) && r.length % 2) continue;
                (void)ann.setData(r);
            }
            ListingOptions o;
            o.from = from;
            o.to = from + size;
            const ListingResult r = renderListing(mem, an, ann, o);
            CHECK(r.verified);
            if (!r.verified) std::cerr << r.problem << "\n";
        }
    }

    return finish("datatypes");
}
