// m6510_dormann - runs Klaus Dormann's 6502 functional test (or another test image of the same kind)
// on m6510::Cpu.
//
//     m6510_dormann <image.bin> <load address> <start address> <success address> [max instructions]
//
// The test programs end in a "trap": a branch or jump to itself. The runner executes until the PC
// does not change any more and reports where it stopped; the test passed if that is the success
// address. The binary is GPL-3.0 and is fetched at run time (CI), never stored in this repository.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "m6510/cpu.hpp"
#include "m6510/image_io.hpp"

using namespace m6510;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: m6510_dormann image.bin load start success [max_instructions]\n");
        return 2;
    }
    auto num = [](const char* s) { return static_cast<unsigned>(std::strtoul(s, nullptr, 0)); };
    const std::vector<Byte> image = readBinaryFile(argv[1]);
    const unsigned load = num(argv[2]), start = num(argv[3]), success = num(argv[4]);
    const std::uint64_t maxInstructions = argc > 5 ? std::strtoull(argv[5], nullptr, 0) : 200000000ULL;

    Memory mem;
    for (std::size_t i = 0; i < image.size() && load + i < 0x10000; ++i) mem.write8(static_cast<Address>(load + i), image[i]);
    Cpu cpu(mem);
    cpu.setPortEnabled(false);
    cpu.registers().pc = static_cast<Address>(start);
    std::uint64_t n = 0;
    Address last = 0xFFFF;
    int same = 0;
    while (n < maxInstructions) {
        const Address pc = cpu.registers().pc;
        if (pc == last) {
            if (++same >= 3) break;  // trapped
        } else {
            same = 0;
            last = pc;
        }
        if (cpu.step().reason == StopReason::Halted) break;
        ++n;
    }
    const Address pc = cpu.registers().pc;
    std::printf("stopped at $%04X after %llu instructions, %llu cycles: %s\n", pc, static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(cpu.cycleCount()), pc == success ? "PASSED" : "FAILED");
    return pc == success ? 0 : 1;
}
