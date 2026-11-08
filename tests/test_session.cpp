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
                              "console", "keys", "screen", "run_program", "help"};
        const char* values[] = {"0", "1", "-5", "\"0xC000\"", "\"$FFFF\"", "\"loop\"", "true", "null", "[]", "{}",
                                "\"A\"", "\"c64\"", "\"generic\"", "70000", "\"\"", "\"LDA #1\\n BRK\""};
        const char* keys[] = {"addr", "len", "n", "source", "hex", "regs", "name", "type", "kind", "text", "max_instructions",
                              "count", "until", "memory", "state", "entry", "origin"};
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

    return finish("session");
}
