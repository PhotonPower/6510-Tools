// m6510_harte - runs Tom Harte's single-step tests (SingleStepTests/65x02, set "6502", MIT licence)
// against m6510::Cpu.
//
//     m6510_harte [--max-fail n] [--limit n] 6502/v1/00.json 6502/v1/01.json ...
//
// Each test gives the initial state (registers, RAM bytes), the final state and the list of bus
// cycles of one instruction. Compared are all registers, every listed RAM byte and the number of
// cycles. The order of the individual bus accesses is not compared (the core does not model bus
// cycles, see docs/emulator.md). JAM opcodes are skipped: the core halts, the test set records the
// bus activity of the locked-up chip. The I/O port of the 6510 is disabled (the tests are for a 6502).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "m6510/cpu.hpp"
#include "m6510/image_io.hpp"
#include "m6510/json.hpp"
#include "m6510/opcodes.hpp"

using namespace m6510;

namespace {

struct Totals {
    std::uint64_t tests = 0, failed = 0, skipped = 0;
};

unsigned num(const Json& o, const char* key) {
    const Json* v = o.find(key);
    if (!v || !v->isNumber()) throw JsonError(std::string("missing field '") + key + "'");
    return static_cast<unsigned>(v->asInt());
}

void runFile(const std::string& path, Totals& total, int maxFail, std::size_t limit) {
    const std::string text = readTextFile(path);
    const Json tests = Json::parse(text);
    std::uint64_t failedHere = 0, testsHere = 0;
    int shown = 0;
    for (const Json& t : tests.items()) {
        if (limit && testsHere >= limit) break;
        const Json& in = *t.find("initial");
        const Json& out = *t.find("final");
        Memory mem;
        for (const Json& cell : in.find("ram")->items())
            mem.write8(static_cast<Address>(cell.items()[0].asInt()), static_cast<Byte>(cell.items()[1].asInt()));
        const Byte opcode = mem.read8(static_cast<Address>(num(in, "pc")));
        if (opInfo(opcode).kind == OpKind::Jam) {
            ++total.skipped;
            continue;
        }
        Cpu cpu(mem);
        cpu.setPortEnabled(false);
        CpuState s = cpu.state();
        s.regs.pc = static_cast<Address>(num(in, "pc"));
        s.regs.s = static_cast<Byte>(num(in, "s"));
        s.regs.a = static_cast<Byte>(num(in, "a"));
        s.regs.x = static_cast<Byte>(num(in, "x"));
        s.regs.y = static_cast<Byte>(num(in, "y"));
        s.regs.p = static_cast<Byte>(num(in, "p"));
        s.irqInhibit = true;
        s.portEnabled = false;
        cpu.setState(s);
        const StepResult r = cpu.step();
        ++testsHere;

        std::string why;
        const Registers& g = cpu.registers();
        auto cmp = [&](const char* name, unsigned got, unsigned want) {
            if (got != want) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " %s=%02X(want %02X)", name, got, want);
                why += buf;
            }
        };
        cmp("PC", g.pc, num(out, "pc"));
        cmp("S", g.s, num(out, "s"));
        cmp("A", g.a, num(out, "a"));
        cmp("X", g.x, num(out, "x"));
        cmp("Y", g.y, num(out, "y"));
        cmp("P", g.p | kFlagU | kFlagB, num(out, "p") | kFlagU | kFlagB);
        for (const Json& cell : out.find("ram")->items()) {
            const auto a = static_cast<Address>(cell.items()[0].asInt());
            const auto want = static_cast<unsigned>(cell.items()[1].asInt());
            if (mem.read8(a) != want) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " [%04X]=%02X(want %02X)", a, mem.read8(a), want);
                why += buf;
            }
        }
        const std::size_t wantCycles = t.find("cycles")->items().size();
        if (r.cycles != wantCycles) {
            why += " cycles=" + std::to_string(r.cycles) + "(want " + std::to_string(wantCycles) + ")";
        }
        if (!why.empty()) {
            ++failedHere;
            if (shown++ < maxFail) std::printf("  FAIL %s: %s\n", t.find("name")->asString().c_str(), why.c_str());
        }
    }
    total.tests += testsHere;
    total.failed += failedHere;
    std::printf("%s: %llu tests, %llu failed\n", path.c_str(), static_cast<unsigned long long>(testsHere),
                static_cast<unsigned long long>(failedHere));
}

}  // namespace

int main(int argc, char** argv) {
    int maxFail = 3;
    std::size_t limit = 0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-fail" && i + 1 < argc) maxFail = std::atoi(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = static_cast<std::size_t>(std::atol(argv[++i]));
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: m6510_harte [--max-fail n] [--limit n] file.json...\n");
        return 2;
    }
    Totals total;
    for (const std::string& f : files) {
        try {
            runFile(f, total, maxFail, limit);
        } catch (const std::exception& e) {
            std::printf("%s: ERROR %s\n", f.c_str(), e.what());
            ++total.failed;
        }
    }
    std::printf("total: %llu tests, %llu failed, %llu skipped (JAM)\n", static_cast<unsigned long long>(total.tests),
                static_cast<unsigned long long>(total.failed), static_cast<unsigned long long>(total.skipped));
    return total.failed == 0 ? 0 : 1;
}
