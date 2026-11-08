// m6510tool - command line front end for the MOS 6510 / Commodore 64 tools.
//
// Version 0.1 is the project skeleton: only `help` and `version` do something. The planned
// sub-commands (asm, dis, monitor, serve, d64, t64, crt, basic) are listed in docs/roadmap.md.

#include <iostream>
#include <string>

#include "m6510/version.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;  // bad command line / I/O problems

void printUsage(std::ostream& out) {
    out << "m6510tool " << m6510::kVersion << " - MOS 6510 / Commodore 64 tools\n"
        << "\n"
        << "Usage:\n"
        << "  m6510tool help | version\n"
        << "\n"
        << "Planned (not implemented yet, see docs/roadmap.md):\n"
        << "  m6510tool asm <source.asm> [options]\n"
        << "  m6510tool dis <image>      [options]\n"
        << "  m6510tool monitor [image]\n"
        << "  m6510tool serve [--allow-files]\n"
        << "  m6510tool d64 info|ls|extract <disk.d64> ...\n"
        << "  m6510tool t64 | crt | basic ...\n";
}

bool isPlanned(const std::string& command) {
    for (const char* c : {"asm", "dis", "monitor", "serve", "d64", "t64", "crt", "basic"}) {
        if (command == c) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(std::cerr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage(std::cout);
        return kExitOk;
    }
    if (command == "version" || command == "--version") {
        std::cout << "m6510tool " << m6510::kVersion << "\n";
        return kExitOk;
    }
    if (isPlanned(command)) {
        std::cerr << "m6510tool: '" << command << "' is not implemented yet (see docs/roadmap.md)\n";
        return kExitUsage;
    }
    std::cerr << "m6510tool: unknown command '" << command << "'\n\n";
    printUsage(std::cerr);
    return kExitUsage;
}
