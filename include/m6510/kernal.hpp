#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "m6510/c64.hpp"
#include "m6510/cpu.hpp"
#include "m6510/image_io.hpp"

namespace m6510 {

/// One call into the KERNAL (docs/c64.md).
struct KernalCall {
    Address address = 0;    ///< entry that was reached (jump table, or the routine behind a RAM vector)
    std::string name;       ///< "CHROUT", or "" for an unknown entry
    bool emulated = false;  ///< false: only logged, returned with C = 0
    Address callSite = 0;   ///< address of the JSR (return address on the stack - 2)
    Registers before, after;
    std::string note;
};

/// High-level emulation of the C64 KERNAL without the ROM (docs/c64.md, "Emulierte KERNAL-Aufrufe").
///
/// `install()` generates an 8 KiB KERNAL image: the jump table at $FF81-$FFF3 (vectored entries as real
/// `JMP ($03xx)` code, so that programs can redirect CHROUT & co. through the RAM vectors), the IRQ/NMI
/// entry code and hardware vectors. An execute hook catches every other address of the KERNAL area while
/// it is visible: known routines are emulated, everything else is logged and returns with C = 0.
class Kernal {
public:
    /// Return address pushed by `startProgram`: a program that returns there has "returned to BASIC".
    static constexpr Address kExitTrap = 0xFFF6;

    Kernal(C64& c64, Cpu& cpu, Memory& ram);
    Kernal(const Kernal&) = delete;
    Kernal& operator=(const Kernal&) = delete;
    ~Kernal();

    /// Generates and installs the KERNAL image and the execute hook.
    void install();
    /// State after power-on, as far as a machine-code program can see it: I/O port $2F/$37, CIA setup
    /// (timer A IRQ at 60 Hz), zero page and vectors, screen cleared, cursor home. BASIC is not emulated.
    void coldStart();
    /// Starts a machine-code program like `SYS address` from BASIC: cold start state, A/X/Y/P from
    /// $030C-$030F, return address = `kExitTrap`, IRQs enabled.
    void startProgram(Address entry);

    /// Text written with CHROUT since the last call (PETSCII control codes as {NAME}).
    std::string takeConsole();
    /// KERNAL calls since the last call.
    std::vector<KernalCall> takeCalls();
    /// Queues keys for GETIN/CHRIN (ASCII; '\n' becomes RETURN).
    void addKeys(const std::string& text);

    /// Supplies files for LOAD (name in ASCII, "*" = first file); nullopt = file not found.
    std::function<std::optional<Program>(const std::string& name, unsigned device)> files;
    /// Files written with SAVE (name -> program).
    std::map<std::string, Program> saved;

    /// Name of a KERNAL entry address (jump table or internal routine), "" if unknown.
    [[nodiscard]] static std::string entryName(Address address);
    /// All jump-table names (for symbols).
    [[nodiscard]] static const std::map<Address, std::string>& jumpTable();

    struct State {
        std::deque<Byte> keys;
    };
    [[nodiscard]] State state() const { return {keys_}; }
    void setState(const State& s) { keys_ = s.keys; }

    std::size_t maxCalls = 4096;

private:
    C64& c64_;
    Cpu& cpu_;
    Memory& ram_;
    std::deque<Byte> keys_;
    std::string console_;
    std::vector<KernalCall> calls_;

    bool hook(Cpu& cpu);
    bool emulate(const std::string& name, Registers& r, std::string& note);
    void chrout(Byte c);
    void scrollUp();
    void clearScreen();
    void restor();
    void ioinit();
    void ramtas();
    void cint();
    [[nodiscard]] std::string fileName() const;
};

}  // namespace m6510
