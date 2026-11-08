// Exhaustive decoder/assembler round trip: whatever the decoder shows as an instruction must
// assemble back to the same bytes.
//
//  - every opcode at CPU level "All";
//  - one-byte operands (immediate, zero page, indirect, branches): all 256 values;
//  - two-byte operands: all 65536 values (M6510_ROUNDTRIP_STEP=n tests every n-th high byte, e.g.
//    under the sanitizers);
//  - branches also near the wrap-around at $0000/$FFFF;
//  - with and without a symbolizer (names for every address, defined as EQU for the assembler).
//
// Non-preferred duplicate opcodes (e.g. NOP $1A) must assemble to the *preferred* opcode with the
// same operand bytes; the disassembler writes them as .BYTE in source mode.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>

#include "m6510/assembler.hpp"
#include "m6510/decoder.hpp"
#include "test_util.hpp"

using namespace m6510;

namespace {

int g_reported = 0;

void check(const Memory& mem, Address pc, bool withSymbols) {
    DecodeOptions o;
    o.level = CpuLevel::All;
    std::map<std::string, std::int64_t> defs;
    if (withSymbols) {
        o.symbolizer = [&defs](Address a) {
            const std::string n = "s_" + hexDigits(a, 4);
            defs[n] = a;
            return n;
        };
    }
    const Instruction in = decode(mem, pc, o);
    ++g_checks;
    if (!in.valid) {
        ++g_failures;
        std::cerr << "opcode " << hexDigits(in.opcode, 2) << " not valid at level All\n";
        return;
    }
    AssemblerOptions ao;
    ao.origin = pc;
    ao.level = CpuLevel::All;
    ao.predefined = defs;
    const AssemblyResult r = assemble(" " + in.text(), ao);
    std::vector<Byte> expect = in.bytes;
    if (!in.canonical) expect[0] = *findOpcode(opInfo(in.opcode).mnemonic, in.mode);
    const std::vector<Byte> got = r.ok() ? r.flatten() : std::vector<Byte>{};
    if (!r.ok() || got != expect || r.lowest() != pc) {
        ++g_failures;
        if (++g_reported <= 20) {
            std::cerr << "round trip failed at " << hexDigits(pc, 4) << ": " << hexBytes(in.bytes) << " -> '" << in.text()
                      << "' -> " << (r.ok() ? hexBytes(got) : r.errors.front().message) << "\n";
        }
    }
}

}  // namespace

int main() {
    std::mt19937 rng(6510);
    unsigned step = 1;
    if (const char* e = std::getenv("M6510_ROUNDTRIP_STEP")) step = std::max(1, std::atoi(e));
    for (int op = 0; op < 256; ++op) {
        const Mode mode = opInfo(static_cast<Byte>(op)).mode;
        const unsigned len = modeLength(mode);
        Memory mem;
        const Address pc = 0x1000;
        mem.write8(pc, static_cast<Byte>(op));
        if (len == 1) {
            check(mem, pc, false);
        } else if (len == 2) {
            for (unsigned v = 0; v < 256; ++v) {
                mem.write8(pc + 1, static_cast<Byte>(v));
                check(mem, pc, false);
                if (v % 16 == 3) check(mem, pc, true);
            }
            if (mode == Mode::Relative) {
                for (Address p : {Address{0x0000}, Address{0x0040}, Address{0xFF80}, Address{0xFFFE}}) {
                    Memory m2;
                    for (unsigned v = 0; v < 256; v += 5) {
                        m2.write8(p, static_cast<Byte>(op));
                        m2.write8(static_cast<Address>(p + 1), static_cast<Byte>(v));
                        check(m2, p, false);
                    }
                }
            }
        } else {
            for (unsigned hi = 0; hi < 256; hi += (hi < 2 || hi > 0xFD) ? 1 : step) {
                for (unsigned lo = 0; lo < 256; ++lo) {
                    mem.write8(pc + 1, static_cast<Byte>(lo));
                    mem.write8(pc + 2, static_cast<Byte>(hi));
                    check(mem, pc, (lo + hi) % 61 == 5);
                }
            }
            for (int k = 0; k < 256; ++k) {
                const auto w = static_cast<unsigned>(rng() & 0xFFFF);
                mem.write8(pc + 1, static_cast<Byte>(w));
                mem.write8(pc + 2, static_cast<Byte>(w >> 8));
                check(mem, pc, k % 4 == 0);
            }
        }
    }
    return finish("roundtrip");
}
