// m6510_dump_forms - prints the opcode table for the cross-check scripts (tools/crosscheck_*.py).
//
// One line per opcode:  OP MNEMONIC MODE LENGTH KIND PREFERRED
// e.g.                  BD LDA abs,X 3 documented 1
//
// With --decode HEXBYTES... it prints the decoder's text for each instruction instead
// (level All, address $1000): "BYTES|TEXT".

#include <cstdio>
#include <cstdlib>
#include <string>

#include "m6510/decoder.hpp"
#include "m6510/opcodes.hpp"

using namespace m6510;

namespace {

const char* kindName(OpKind k) {
    switch (k) {
        case OpKind::Documented: return "documented";
        case OpKind::Illegal: return "illegal";
        case OpKind::Unstable: return "unstable";
        case OpKind::Jam: return "jam";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--decode") {
        for (int i = 2; i < argc; ++i) {
            const std::string hex = argv[i];
            Memory m;
            Address a = 0x1000;
            for (std::size_t k = 0; k + 1 < hex.size(); k += 2) m.write8(a++, static_cast<Byte>(std::strtoul(hex.substr(k, 2).c_str(), nullptr, 16)));
            DecodeOptions o;
            o.level = CpuLevel::All;
            std::printf("%s|%s\n", hex.c_str(), decode(m, 0x1000, o).text().c_str());
        }
        return 0;
    }
    for (int op = 0; op < 256; ++op) {
        const OpInfo& i = opInfo(static_cast<Byte>(op));
        std::printf("%02X %s %s %u %s %d\n", op, i.mnemonic, modeName(i.mode), modeLength(i.mode), kindName(i.kind),
                    i.preferred ? 1 : 0);
    }
    return 0;
}
