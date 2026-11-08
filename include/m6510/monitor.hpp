#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "m6510/machine.hpp"

namespace m6510 {

struct MonitorOptions {
    bool allowFiles = true;                      ///< l, sv, asm, symload, symsave, savestate, loadstate, rom
    std::uint64_t maxInstructions = 10'000'000;  ///< per run command (`limit` changes it)
};

/// Interactive machine-code monitor: memory, disassembler, assembler and the emulator's debugger
/// functions behind a classic one-line command interface (docs/monitor.md).
///
/// `execute` takes one input line and returns the text to print; it never throws and has no I/O of its
/// own apart from the file commands, so the same class serves the terminal front end
/// (`m6510tool monitor`) and the tests. Numbers in arguments are **hexadecimal** (`C000`, `FF`); other
/// bases and expressions through prefixes (`$FF`, `0xFF`, `%1010`), a trailing dot (`100.` = decimal),
/// symbols (`start+3`) and registers (`@A`, `@PC`). Lines typed in assemble mode use the assembler's
/// syntax (decimal by default).
class Monitor {
public:
    explicit Monitor(MonitorOptions options = {});
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    ~Monitor();

    /// Executes one command line. Returns the output (empty or ending in a newline).
    std::string execute(std::string_view line);

    /// Prompt to show before reading the next line ("-" normally, "C000:" in assemble mode).
    [[nodiscard]] std::string prompt() const;
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] static std::string banner();

    [[nodiscard]] Machine& machine() { return m_; }
    [[nodiscard]] Cpu& cpu() { return m_.cpu(); }

private:
    struct Impl;
    friend struct Impl;

    MonitorOptions options_;
    Machine m_;
    Address nextDisassembly_ = 0;
    Address nextDump_ = 0;
    std::optional<Address> assembleAt_;
    std::uint64_t limit_;
    bool finished_ = false;
};

}  // namespace m6510
