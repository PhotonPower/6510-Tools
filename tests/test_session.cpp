#include "m6510/session.hpp"

#include <cstdio>
#include <random>

#include "m6510/image_io.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

Json call(Session& s, const std::string& line) {
    const std::string reply = s.handleLine(line);
    return Json::parse(reply);
}

bool ok(const Json& j) {
    const Json* o = j.find("ok");
    return o && o->isBool() && o->asBool();
}

std::string str(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : std::string("<missing ") + key + ">";
}

std::int64_t num(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isNumber() ? v->asInt() : -1;
}

const Json& sub(const Json& j, const char* key) {
    static const Json empty;
    const Json* v = j.find(key);
    return v ? *v : empty;
}

}  // namespace

int main() {
    // ---- basics on the generic machine ------------------------------------------------------------
    {
        Session s;
        Json r = call(s, R"({"cmd":"asm","source":"start: LDA #5\n ASL A\n STA $0400\nloop: INX\n JMP loop","origin":"0xC000","entry":"start"})");
        CHECK(ok(r));
        CHECK_EQ(num(r, "size"), 10);
        CHECK_EQ(str(r, "start"), std::string("0xC000"));
        CHECK_EQ(str(sub(r, "symbols"), "loop"), std::string("0xC006"));
        r = call(s, R"({"cmd":"step","n":3})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("stepped"));
        CHECK_EQ(str(sub(r, "regs"), "A"), std::string("0x0A"));
        r = call(s, R"({"cmd":"read","addr":"$0400","len":1})");
        CHECK_EQ(str(r, "hex"), std::string("0A"));
        r = call(s, R"({"cmd":"run","max_instructions":100})");
        CHECK_EQ(str(r, "stop"), std::string("instruction_limit"));
        CHECK_EQ(num(r, "instructions"), 100);
        r = call(s, R"({"cmd":"break","addr":"loop+1"})");
        CHECK(ok(r));
        r = call(s, R"({"cmd":"run"})");
        CHECK_EQ(str(r, "stop"), std::string("breakpoint"));
        CHECK_EQ(str(r, "pc"), std::string("0xC007"));
        r = call(s, R"({"cmd":"breaks"})");
        CHECK_EQ(sub(r, "breakpoints").items().size(), std::size_t{1});
        CHECK(ok(call(s, R"({"cmd":"clear_breaks"})")));
        r = call(s, R"({"cmd":"dis","addr":"start","count":3})");
        CHECK(ok(r));
        CHECK_EQ(str(sub(r, "lines").items()[0], "text"), std::string("LDA #$05"));
        CHECK_EQ(str(sub(r, "lines").items()[0], "label"), std::string("start"));
        r = call(s, R"({"cmd":"set","regs":{"A":"$7F","X":3,"PC":"start","P":"0x01"}})");
        CHECK(ok(r));
        CHECK_EQ(str(sub(r, "regs"), "X"), std::string("0x03"));
        CHECK_EQ(str(sub(r, "regs"), "flags"), std::string("..-....C"));
        r = call(s, R"({"cmd":"write","addr":"0x2000","hex":"A9 01 60"})");
        CHECK_EQ(num(r, "written"), 3);
        r = call(s, R"({"cmd":"write","addr":"0x2003","bytes":[1,"$FF",-1]})");
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0x2000","len":6})"), "hex"), std::string("A9 01 60 01 FF FF"));
        r = call(s, R"({"cmd":"symbols"})");
        CHECK_EQ(str(sub(r, "symbols"), "start"), std::string("0xC000"));
    }

    // ---- errors never crash and change nothing --------------------------------------------------------
    {
        Session s;
        for (const char* bad : {"", "nonsense", "[1,2]", R"({"cmd":42})", R"({"cmd":"nope"})", R"({"cmd":"read"})",
                                R"({"cmd":"read","addr":"undefined_sym"})", R"({"cmd":"read","addr":1,"len":5000})",
                                R"({"cmd":"run","max_instructions":200000000})", R"({"cmd":"step","n":-1})",
                                R"({"cmd":"set","regs":{"Q":1}})", R"({"cmd":"set","regs":{"A":300}})",
                                R"({"cmd":"write","addr":0,"hex":"ABC"})", R"({"cmd":"rollback","name":"x"})",
                                R"({"cmd":"io"})", R"({"cmd":"screen"})", R"({"cmd":"machine","type":"vic20"})",
                                R"({"cmd":"load_file","file":"x.prg"})", R"({"cmd":"save_state","file":"x"})",
                                R"({"cmd":"watch","addr":1,"kind":"exec"})", R"({"cmd":"interrupt","kind":"reset"})"}) {
            const Json r = call(s, bad);
            CHECK(!ok(r));
            CHECK(!str(r, "error").empty());
        }
        const Json r = call(s, R"({"cmd":"asm","source":" LDA #1\n FOO\n LDA #300"})");
        CHECK(!ok(r));
        CHECK_EQ(sub(r, "errors").items().size(), std::size_t{2});
        CHECK_EQ(str(sub(r, "errors").items()[0], "source"), std::string(" FOO"));
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0xC000","len":2})"), "hex"), std::string("00 00"));  // nothing written
    }

    // ---- call: returned only for a real return --------------------------------------------------------
    {
        Session s;
        call(s, R"({"cmd":"asm","source":"double: ASL A\n RTS\nbad: PLA\n PLA\n PLA\n RTS","origin":"0x1000"})");
        Json r = call(s, R"({"cmd":"call","addr":"double","regs":{"A":21}})");
        CHECK(ok(r));
        CHECK_EQ(sub(r, "returned").asBool(), true);
        CHECK_EQ(str(sub(r, "regs"), "A"), std::string("0x2A"));
        r = call(s, R"({"cmd":"call","addr":"bad","max_instructions":1000})");
        CHECK_EQ(sub(r, "returned").asBool(), false);
        // memory set-up for the call
        call(s, R"({"cmd":"asm","source":"sum: LDA $FB\n CLC\n ADC $FC\n RTS","origin":"0x1100"})");
        r = call(s, R"({"cmd":"call","addr":"sum","memory":{"0xFB":"05 07"}})");
        CHECK_EQ(str(sub(r, "regs"), "A"), std::string("0x0C"));
    }

    // ---- checkpoint -> run -> rollback -> run gives identical answers ---------------------------------
    {
        Session s;
        call(s, R"({"cmd":"asm","source":"l: INC $2000\n LDA $2000\n ADC $2001\n STA $2001\n DEX\n BNE l\n BRK","entry":"0xC000"})");
        CHECK(ok(call(s, R"({"cmd":"checkpoint","name":"fresh"})")));
        const std::string first = s.handleLine(R"({"cmd":"run","max_instructions":5000})");
        const Json d = call(s, R"({"cmd":"diff","name":"fresh"})");
        CHECK(num(d, "changed_bytes") >= 2);
        bool saw2000 = false;
        for (const Json& rg : sub(d, "ranges").items()) saw2000 = saw2000 || str(rg, "addr") == "0x2001";  // $2000 is back at 0 after 256 INCs
        CHECK(saw2000);
        CHECK(ok(call(s, R"({"cmd":"rollback","name":"fresh"})")));
        const std::string second = s.handleLine(R"({"cmd":"run","max_instructions":5000})");
        CHECK_EQ(second, first);
        const Json cps = call(s, R"({"cmd":"checkpoints"})");
        CHECK_EQ(sub(cps, "checkpoints").items().size(), std::size_t{1});
        CHECK(ok(call(s, R"({"cmd":"drop","name":"fresh"})")));
        CHECK(!ok(call(s, R"({"cmd":"rollback","name":"fresh"})")));
    }

    // ---- watchpoints, trace, step_over, run_until_return -----------------------------------------------
    {
        Session s;
        call(s, R"({"cmd":"asm","source":" JSR sub\n STA $3000\n BRK\nsub: LDA #9\n RTS","entry":"0xC000"})");
        Json r = call(s, R"({"cmd":"step_over"})");
        CHECK_EQ(str(r, "pc"), std::string("0xC003"));
        CHECK(ok(call(s, R"({"cmd":"watch","addr":"0x3000"})")));
        r = call(s, R"({"cmd":"run"})");
        CHECK_EQ(str(r, "stop"), std::string("watchpoint"));
        CHECK_EQ(str(sub(r, "watch"), "addr"), std::string("0x3000"));
        CHECK(ok(call(s, R"({"cmd":"unwatch","addr":"0x3000"})")));
        call(s, R"({"cmd":"set","regs":{"PC":"0xC000"}})");
        r = call(s, R"({"cmd":"step","n":3,"trace":true})");
        CHECK_EQ(sub(r, "trace").items().size(), std::size_t{3});
        CHECK_EQ(str(sub(r, "trace").items()[1], "text"), std::string("LDA #$09"));
        call(s, R"({"cmd":"set","regs":{"PC":"0xC000"}})");
        call(s, R"({"cmd":"step"})");
        r = call(s, R"({"cmd":"run_until_return"})");
        CHECK_EQ(str(r, "pc"), std::string("0xC003"));
        CHECK_EQ(str(r, "stop"), std::string("returned"));
    }

    // ---- C64 machine with KERNAL emulation --------------------------------------------------------
    {
        Session s;
        Json r = call(s, R"({"cmd":"machine","type":"c64"})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "kernal"), std::string("emulated"));
        r = call(s, R"({"cmd":"asm","source":" * = $0801\n .BASIC\nstart: LDX #0\nl: LDA msg,X\n BEQ d\n JSR CHROUT\n INX\n BNE l\nd: LDA #2\n STA $D020\n RTS\nmsg: .BYTE $93,\"HI\",0"})");
        CHECK(ok(r));
        CHECK(ok(call(s, R"({"cmd":"checkpoint","name":"loaded"})")));
        r = call(s, R"({"cmd":"run_program"})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK_EQ(str(r, "message"), std::string("program returned to BASIC"));
        CHECK_EQ(str(r, "entry"), std::string("0x080D"));
        CHECK_EQ(str(call(s, R"({"cmd":"console"})"), "text"), std::string("{CLR}HI"));
        r = call(s, R"({"cmd":"io"})");
        CHECK_EQ(sub(r, "output").items().size(), std::size_t{1});
        CHECK_EQ(str(sub(r, "output").items()[0], "what"), std::string("EXTCOL"));
        r = call(s, R"({"cmd":"kernalcalls"})");
        CHECK_EQ(sub(r, "calls").items().size(), std::size_t{3});
        CHECK_EQ(str(sub(r, "calls").items()[0], "name"), std::string("CHROUT"));
        r = call(s, R"({"cmd":"screen"})");
        CHECK_EQ(sub(r, "lines").items()[0].asString(), std::string("HI"));
        CHECK_EQ(num(r, "border"), 2);
        CHECK_EQ(str(r, "mode"), std::string("text"));
        // rollback restores colour RAM, chips and the screen
        CHECK(ok(call(s, R"({"cmd":"rollback","name":"loaded"})")));
        CHECK_EQ(num(call(s, R"({"cmd":"screen"})"), "border"), 14);
        // keys for GETIN
        call(s, R"({"cmd":"asm","source":"w: JSR GETIN\n BEQ w\n STA $C100\n RTS","origin":"0xC000"})");
        CHECK(ok(call(s, R"({"cmd":"keys","text":"a"})")));
        r = call(s, R"({"cmd":"run_program","addr":"w"})");
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0xC100","len":1})"), "hex"), std::string("41"));
        // the CPU view and the RAM view differ under the KERNAL
        CHECK(ok(call(s, R"({"cmd":"write","addr":"0xE000","hex":"55"})")));
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0xE000","len":1,"view":"ram"})"), "hex"), std::string("55"));
        // an IRQ requested from outside goes through the KERNAL's IRQ entry to ($0314)
        call(s, R"({"cmd":"asm","source":"irq: INC $C200\n JMP $EA81\nmain: SEI\n LDA #<irq\n STA $0314\n LDA #>irq\n STA $0315\n LDA #$7F\n STA $DC0D\n CLI\nloop: JMP loop","origin":"0xC000"})");
        call(s, R"({"cmd":"set","regs":{"PC":"main"}})");
        call(s, R"({"cmd":"run","max_instructions":20})");
        CHECK(ok(call(s, R"({"cmd":"interrupt","kind":"irq"})")));
        call(s, R"({"cmd":"run","max_instructions":20})");
        CHECK(ok(call(s, R"({"cmd":"interrupt","kind":"irq","release":true})")));
        r = call(s, R"({"cmd":"read","addr":"0xC200","len":1})");
        CHECK(str(r, "hex") != std::string("00"));
    }

    // ---- files: only with --allow-files -------------------------------------------------------------
    {
        const std::string prg = "m6510_session_test.prg", snap = "m6510_session_test.snap";
        AssemblerOptions o;
        writeBinaryFile(prg, makePrg(0x0801, {0x0B, 0x08, 0x0A, 0x00, 0x9E, 0x32, 0x30, 0x36, 0x31, 0x00, 0x00, 0x00, 0xEE, 0x20, 0xD0, 0x60}));
        Session locked;
        CHECK(!ok(call(locked, R"({"cmd":"load_file","file":"m6510_session_test.prg"})")));
        SessionOptions so;
        so.allowFiles = true;
        Session s(so);
        call(s, R"({"cmd":"machine","type":"c64"})");
        Json r = call(s, R"({"cmd":"load_file","file":"m6510_session_test.prg"})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "entry"), std::string("0x080D"));
        CHECK_EQ(str(r, "basic"), std::string("10 SYS2061"));
        r = call(s, R"({"cmd":"run_program"})");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK(ok(call(s, R"({"cmd":"save_state","file":"m6510_session_test.snap"})")));
        const std::string before = s.handleLine(R"({"cmd":"state"})");
        call(s, R"({"cmd":"reset","memory":true})");
        CHECK(ok(call(s, R"({"cmd":"load_state","file":"m6510_session_test.snap"})")));
        CHECK_EQ(s.handleLine(R"({"cmd":"state"})"), before);
        CHECK_EQ(num(call(s, R"({"cmd":"screen"})"), "border"), 15);  // INC $D020 from 14
        // LOAD through an attached file
        CHECK(ok(call(s, R"({"cmd":"attach","file":"m6510_session_test.prg"})")));
        call(s, R"({"cmd":"asm","source":"go: LDA #1\n LDX #<n\n LDY #>n\n JSR SETNAM\n LDA #1\n LDX #8\n LDY #1\n JSR SETLFS\n LDA #0\n JSR LOAD\n RTS\nn: .BYTE \"*\"","origin":"0xC000"})");
        call(s, R"({"cmd":"write","addr":"0x080D","hex":"00"})");
        r = call(s, R"({"cmd":"run_program","addr":"go"})");
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0x080D","len":1})"), "hex"), std::string("EE"));
        // a generic machine refuses a C64 snapshot
        Session g(so);
        CHECK(!ok(call(g, R"({"cmd":"load_state","file":"m6510_session_test.snap"})")));
        // snapshots: every truncation is rejected
        const std::vector<Byte> file = readBinaryFile(snap);
        for (std::size_t cut = 0; cut < file.size(); cut += (cut < 200 ? 1 : 4099)) {
            bool threw = false;
            try {
                (void)deserializeSnapshot(std::vector<Byte>(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut)));
            } catch (const IoError&) {
                threw = true;
            }
            CHECK(threw);
        }
        std::remove(prg.c_str());
        std::remove(snap.c_str());
    }

    // ---- robustness: random commands with random arguments ----------------------------------------
    {
        Session s;
        std::mt19937 rng(77);
        const char* cmds[] = {"asm", "dis", "read", "write", "regs", "set", "step", "step_over", "run", "run_until_return",
                              "call", "break", "unbreak", "breaks", "watch", "unwatch", "checkpoint", "rollback", "diff",
                              "state", "set_state", "interrupt", "reset", "symbols", "machine", "io", "kernalcalls",
                              "console", "keys", "screen", "screenshot", "screens", "screen_diff", "screen_drop", "run_program", "help",
                              "analyze", "routines", "references", "listing", "jump_tables", "data_regions", "ann_label",
                              "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_constant", "ann_remove", "ann_title",
                              "ann_list"};
        const char* values[] = {"0", "1", "-5", "\"0xC000\"", "\"$FFFF\"", "\"loop\"", "true", "null", "[]", "{}",
                                "\"A\"", "\"c64\"", "\"generic\"", "70000", "\"\"", "\"LDA #1\\n BRK\""};
        const char* keys[] = {"addr", "len", "n", "source", "hex", "regs", "name", "type", "kind", "text", "max_instructions",
                              "count", "until", "memory", "state", "entry", "origin", "crop", "zoom", "border", "a", "b", "palette",
                              "from", "to", "entries", "linear", "apply", "order", "kind", "length", "partner", "title", "size",
                              "block", "max_chars", "verify"};
        for (int i = 0; i < 3000; ++i) {
            std::string line = std::string("{\"cmd\":\"") + cmds[rng() % (sizeof(cmds) / sizeof(*cmds))] + "\"";
            const int nargs = static_cast<int>(rng() % 4);
            for (int k = 0; k < nargs; ++k) {
                line += std::string(",\"") + keys[rng() % (sizeof(keys) / sizeof(*keys))] + "\":" +
                        values[rng() % (sizeof(values) / sizeof(*values))];
            }
            line += ",\"max_instructions\":2000}";
            const Json r = Json::parse(s.handleLine(line));
            CHECK(r.find("ok") != nullptr);
            if (!ok(r)) CHECK(!str(r, "error").empty());
        }
    }

    // setting P applies the new I flag at once: an IRQ is taken by the very next step
    {
        Session s;
        call(s, R"({"cmd":"write","addr":"0xFFFE","hex":"00 C0"})");
        call(s, R"({"cmd":"write","addr":"0x2000","hex":"EA EA"})");
        call(s, R"({"cmd":"set","regs":{"PC":"0x2000","P":"0x20"}})");
        call(s, R"({"cmd":"interrupt","kind":"irq"})");
        const Json r = call(s, R"({"cmd":"step"})");
        CHECK_EQ(str(r, "pc"), std::string("0xC000"));
        CHECK_EQ(num(r, "interrupts"), 1);
    }

    // screenshots
    {
        Session s;
        CHECK(!ok(call(s, R"({"cmd":"screenshot"})")));  // generic machine
        call(s, R"({"cmd":"machine","type":"c64"})");
        Json r = call(s, R"({"cmd":"screenshot"})");
        CHECK(ok(r));
        CHECK_EQ(num(r, "width"), 320);
        CHECK_EQ(num(r, "height"), 200);
        CHECK_EQ(str(r, "name"), std::string("last"));
        CHECK_EQ(str(r, "mode"), std::string("text"));
        CHECK_EQ(str(r, "charset"), std::string("replacement"));
        CHECK_EQ(str(r, "png_base64").substr(0, 8), std::string("iVBORw0K"));  // PNG signature
        r = call(s, R"({"cmd":"screenshot","name":"before","border":true,"palette":"pepto"})");
        CHECK_EQ(num(r, "width"), 384);
        r = call(s, R"({"cmd":"screenshot","crop":{"col0":0,"row0":0,"col1":3,"row1":1}})");
        CHECK_EQ(num(r, "width"), 32 * 8);  // 4 cells = 32 px, zoom 320/32 -> 8 (at most)
        CHECK_EQ(num(r, "zoom"), 8);
        r = call(s, R"({"cmd":"screenshot","crop":{"col0":0,"row0":0,"col1":39,"row1":24},"zoom":2})");
        CHECK_EQ(num(r, "width"), 640);
        CHECK(!ok(call(s, R"({"cmd":"screenshot","crop":{"col0":5,"row0":0,"col1":3,"row1":1}})")));
        CHECK(!ok(call(s, R"({"cmd":"screenshot","crop":{"col0":0,"row0":0,"col1":40,"row1":1}})")));
        CHECK(!ok(call(s, R"({"cmd":"screenshot","crop":[1,2]})")));
        CHECK(!ok(call(s, R"({"cmd":"screenshot","zoom":9})")));
        CHECK(!ok(call(s, R"({"cmd":"screenshot","palette":"vice"})")));
        call(s, R"({"cmd":"screenshot","name":"a"})");
        r = call(s, R"({"cmd":"screens"})");
        CHECK_EQ(r.find("screens")->items().size(), std::size_t{3});  // last, before, a
        r = call(s, R"({"cmd":"screen_diff","a":"a"})");
        CHECK_EQ(num(r, "changed_pixels"), 0);
        call(s, R"({"cmd":"write","addr":"0x0400","hex":"A0"})");  // reversed space in the top left cell
        call(s, R"({"cmd":"write","addr":"0xD800","hex":"01"})");
        r = call(s, R"({"cmd":"screen_diff","a":"a"})");
        CHECK_EQ(num(r, "changed_pixels"), 64);
        CHECK_EQ(num(*r.find("text_cells"), "col1"), 0);
        CHECK_EQ(num(*r.find("bbox"), "x1"), 7);
        r = call(s, R"({"cmd":"screen_diff","a":"a","b":"before"})");
        CHECK(!r.find("same_size")->asBool());  // 320 x 200 against 384 x 272
        CHECK(!ok(call(s, R"({"cmd":"screen_diff","a":"nosuch"})")));
        CHECK(ok(call(s, R"({"cmd":"screen_drop","name":"a"})")));
        CHECK(!ok(call(s, R"({"cmd":"screen_drop","name":"a"})")));
    }

    // reverse engineering: analyze, routines, tables, data, references, annotations, listing
    {
        Session s;
        CHECK(!ok(call(s, R"({"cmd":"analyze"})")));  // nothing loaded
        CHECK(!ok(call(s, R"({"cmd":"listing"})")));  // no analysis
        call(s, R"({"cmd":"machine","type":"c64"})");
        const char* src = R"({"cmd":"asm","origin":"0xC000","source":"start: LDA #<msg\n LDY #>msg\n JSR print\n LDX #1\n JSR dispatch\n RTS\nprint: STA $FB\n STY $FC\n LDY #0\nl: LDA ($FB),Y\n BEQ d\n JSR CHROUT\n INY\n BNE l\nd: RTS\ndispatch: LDA hi,X\n PHA\n LDA lo,X\n PHA\n RTS\nc0: INC $D020\n RTS\nc1: STA patch+1\npatch: LDA #0\n RTS\nlo: .LO c0-1, c1-1\nhi: .HI c0-1, c1-1\nmsg: .BYTE \"HELLO\", 0","entry":"start"})";
        CHECK(ok(call(s, src)));
        Json r = call(s, R"({"cmd":"analyze"})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "from"), std::string("0xC000"));
        CHECK_EQ(num(r, "routines"), 5);
        CHECK_EQ(num(r, "jump_tables"), 1);
        r = call(s, R"({"cmd":"jump_tables"})");
        const Json& t = r.find("tables")->items()[0];
        CHECK_EQ(str(t, "kind"), std::string("rts"));
        CHECK(t.find("minus_one")->asBool());
        CHECK_EQ(num(t, "entry_count"), 2);
        r = call(s, R"({"cmd":"routines","order":"address"})");
        const auto& rs = r.find("routines")->items();
        CHECK_EQ(rs.size(), std::size_t{5});
        bool foundChrout = false, foundIo = false;
        for (const Json& x : rs) {
            for (const Json& k : x.find("kernal_calls")->items()) foundChrout |= str(k, "name") == "CHROUT";
            for (const Json& w : x.find("io_access")->find("writes")->items()) foundIo |= str(w, "name") == "EXTCOL";
        }
        CHECK(foundChrout);
        CHECK(foundIo);
        CHECK(!ok(call(s, R"({"cmd":"routines","order":"random"})")));
        r = call(s, R"({"cmd":"data_regions","apply":true})");
        CHECK_EQ(num(r, "applied"), 1);
        CHECK_EQ(str(r.find("regions")->items()[0], "kind"), std::string("text"));
        r = call(s, R"({"cmd":"references","from":"start","to":"print"})");
        bool textRef = false;
        for (const Json& x : r.find("references")->items()) textRef |= str(x, "kind") == "text" && str(x, "preview") == "HELLO";
        CHECK(textRef);
        r = call(s, R"({"cmd":"references","from":"c1","to":"lo"})");
        bool selfmod = false;
        for (const Json& x : r.find("references")->items()) selfmod |= str(x, "kind") == "selfmod";
        CHECK(selfmod);
        // annotations
        CHECK(ok(call(s, R"({"cmd":"ann_label","addr":"print","name":"print_string"})")));
        CHECK(!ok(call(s, R"({"cmd":"ann_label","addr":"start","name":"print_string"})")));  // duplicate
        CHECK(!ok(call(s, R"({"cmd":"ann_label","addr":"start","name":"INY"})")));
        CHECK(ok(call(s, R"({"cmd":"ann_routine","addr":"print","summary":"prints a string","inputs":"A/Y = address","confidence":"high"})")));
        CHECK(!ok(call(s, R"({"cmd":"ann_routine","addr":"print","confidence":"sure"})")));
        CHECK(ok(call(s, R"({"cmd":"ann_variable","addr":"0xFB","name":"text_ptr","size":2})")));
        CHECK(ok(call(s, R"({"cmd":"ann_comment","addr":"start","text":"entry"})")));
        CHECK(!ok(call(s, R"({"cmd":"ann_comment","addr":"0xC001","text":"inside"})")));  // operand byte
        CHECK(!ok(call(s, R"({"cmd":"ann_data","addr":"start","length":2})")));          // code
        CHECK(!ok(call(s, R"({"cmd":"ann_data","addr":"msg","length":2,"kind":"floats"})")));
        CHECK(!ok(call(s, R"({"cmd":"ann_constant","addr":"0xC001"})")));
        CHECK(ok(call(s, R"({"cmd":"ann_constant","addr":"start"})")));
        CHECK(ok(call(s, R"({"cmd":"ann_title","title":"session test"})")));
        r = call(s, R"({"cmd":"listing","addresses":true})");
        CHECK(r.find("verified")->asBool());
        const std::string text = str(r, "listing");
        CHECK(text.find("; session test") != std::string::npos);
        CHECK(text.find("print_string:") != std::string::npos);
        CHECK(text.find("STA text_ptr") != std::string::npos);
        CHECK(text.find("LDA #$") != std::string::npos);  // constant: no #<msg
        CHECK(text.find("LDY #>data_") != std::string::npos);
        CHECK(text.find(".BYTE \"HELLO\",$00") != std::string::npos);
        CHECK_EQ(num(r, "user_comments"), 1);
        r = call(s, R"({"cmd":"listing","max_chars":100})");
        CHECK(r.find("truncated")->asBool());
        CHECK(!ok(call(s, R"({"cmd":"listing","from":"0xB000","to":"0xC010"})")));
        r = call(s, R"({"cmd":"routines","undocumented_only":true})");
        CHECK_EQ(r.find("routines")->items().size(), std::size_t{4});
        r = call(s, R"({"cmd":"ann_list"})");
        CHECK_EQ(str(*r.find("annotations"), "title"), std::string("session test"));
        CHECK(ok(call(s, R"({"cmd":"ann_remove","addr":"print"})")));
        CHECK(!call(s, R"({"cmd":"ann_remove","addr":"print"})").find("removed")->asBool());
        CHECK(!ok(call(s, R"({"cmd":"ann_save","file":"x.json"})")));  // no file access
        // analyze with explicit entries and a linear range
        r = call(s, R"({"cmd":"analyze","reset":true,"entries":["c0"],"linear":{"from":"start","to":"print"}})");
        CHECK(ok(r));
        CHECK(!r.find("default_entry"));
        CHECK(!ok(call(s, R"({"cmd":"analyze","from":"0xC010","to":"0xC000"})")));
        CHECK(!ok(call(s, R"({"cmd":"analyze","entries":"start"})")));
        // a machine change clears the analysis and the annotations
        call(s, R"({"cmd":"machine","type":"generic"})");
        CHECK(!ok(call(s, R"({"cmd":"routines"})")));
        CHECK_EQ(call(s, R"({"cmd":"ann_list"})").find("annotations")->find("labels")->items().size(), std::size_t{0});
    }
    // annotation files
    {
        SessionOptions so;
        so.allowFiles = true;
        Session s(so);
        call(s, R"({"cmd":"asm","origin":"0x1000","source":"main: RTS"})");
        call(s, R"({"cmd":"ann_label","addr":"0x1000","name":"entry_point"})");
        CHECK(ok(call(s, R"({"cmd":"ann_save","file":"m6510_session_test.ann.json"})")));
        call(s, R"({"cmd":"ann_remove","addr":"0x1000"})");
        const Json r = call(s, R"({"cmd":"ann_load","file":"m6510_session_test.ann.json"})");
        CHECK_EQ(num(r, "labels"), 1);
        writeTextFile("m6510_session_test.ann.json", R"({"labels":[{"addr":"0x1000","name":"RTS"}]})");
        CHECK(!ok(call(s, R"({"cmd":"ann_load","file":"m6510_session_test.ann.json"})")));
        writeTextFile("m6510_session_test.ann.json", "{not json");
        CHECK(!ok(call(s, R"({"cmd":"ann_load","file":"m6510_session_test.ann.json"})")));
        std::remove("m6510_session_test.ann.json");
    }

    // call with an interrupt frame: RTI returns to the return address
    {
        Session s;
        call(s, R"({"cmd":"asm","origin":"0xC000","source":"rti_h: INC $10\n RTI\nirq_h: INC $10\n PLA\n TAY\n PLA\n TAX\n PLA\n RTI"})");
        Json r = call(s, R"({"cmd":"call","addr":"rti_h","frame":"rti"})");
        CHECK(r.find("returned")->asBool());
        r = call(s, R"({"cmd":"call","addr":"irq_h","frame":"irq","regs":{"A":1,"X":2,"Y":3}})");
        CHECK(r.find("returned")->asBool());
        CHECK_EQ(str(*r.find("regs"), "X"), std::string("0x02"));
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0x10","len":1})"), "hex"), std::string("02"));
        r = call(s, R"({"cmd":"call","addr":"rti_h"})");  // as a subroutine: RTI pulls the wrong things
        CHECK(!r.find("returned")->asBool());
        CHECK(!ok(call(s, R"({"cmd":"call","addr":"rti_h","frame":"nmi"})")));
    }

    // input, execution tracking, analysis with executed addresses as hints
    {
        Session s;
        CHECK(!ok(call(s, R"({"cmd":"input","joystick2":"fire"})")));  // generic machine
        call(s, R"({"cmd":"machine","type":"c64"})");
        Json r = call(s, R"({"cmd":"input","joystick2":"up,fire","keys":["SPACE","a"]})");
        CHECK_EQ(str(r, "joystick2"), std::string("up,fire"));
        CHECK_EQ(r.find("keys")->items().size(), std::size_t{2});
        CHECK_EQ(str(call(s, R"({"cmd":"read","addr":"0xDC00","len":1})"), "hex"), std::string("EE"));
        CHECK(!ok(call(s, R"({"cmd":"input","joystick1":"sideways"})")));
        CHECK(!ok(call(s, R"({"cmd":"input","keys":["NOKEY"]})")));
        call(s, R"({"cmd":"input","joystick2":"","keys":[]})");
        // a jump through a table the analysis cannot see: only the executed addresses find the target
        call(s, R"({"cmd":"asm","origin":"0xC000","source":"start: LDA #<target\n STA $FB\n LDA #>target\n STA $FC\n LDA $FB\n PHA\n JMP ($00FB)\nfill: .BYTE 0,0,0\ntarget: INC $D020\n RTS","entry":"start"})");
        CHECK(!ok(call(s, R"({"cmd":"analyze","use_executed":true})")));
        call(s, R"({"cmd":"track_execution"})");
        call(s, R"({"cmd":"call","addr":"start"})");
        r = call(s, R"({"cmd":"executed"})");
        CHECK(num(r, "instructions") >= 9);
        r = call(s, R"({"cmd":"analyze"})");
        const std::int64_t before = num(r, "code_bytes");
        r = call(s, R"({"cmd":"analyze","use_executed":true})");
        CHECK(num(r, "code_bytes") > before);
        CHECK(num(r, "executed_hints") >= 9);
        r = call(s, R"({"cmd":"routines","order":"address"})");
        bool targetIsRoutine = false;
        for (const Json& x : r.find("routines")->items()) targetIsRoutine |= str(x, "addr") == "0xC011";
        CHECK(targetIsRoutine);
        CHECK(!call(s, R"({"cmd":"track_execution","on":false})").find("tracking")->asBool());
    }

    return finish("session");
}
