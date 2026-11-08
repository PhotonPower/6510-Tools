#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "m6510/c64.hpp"
#include "m6510/cpu.hpp"
#include "m6510/image_io.hpp"
#include "m6510/kernal.hpp"
#include "m6510/memory.hpp"
#include "m6510/snapshot.hpp"

namespace m6510 {

/// A watched address range (see Cpu::addWatchpoint).
struct WatchSpec {
    Address start = 0;
    unsigned length = 1;
    bool read = false, write = true;
};

/// What `Machine::loadFile` loaded.
struct LoadedFile {
    Program program;
    Address entry = 0;  ///< load address, or the SYS target of a BASIC starter at $0801
    std::string basic;  ///< first BASIC line ("10 SYS 2064") if there is a starter
    std::string source; ///< where it came from, for messages ("disk.d64: \"NAME\" (PRG, 12 blocks)")
};

/// One emulated machine as the session (`m6510tool serve`) and the monitor use it: memory, processor,
/// optionally the C64 memory map and the KERNAL emulation, plus symbols, named checkpoints and
/// watchpoints. Errors are reported as exceptions (std::runtime_error and derived types).
class Machine {
public:
    Machine();  ///< generic machine
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    ~Machine();

    /// Replaces the machine: generic (64 KiB RAM, no I/O port, S = $FF) or C64 with the KERNAL
    /// emulation (`kernalEmulation`) or without a KERNAL (load one with loadRom). Clears memory,
    /// symbols, checkpoints, watchpoints and the attached files.
    void select(bool c64, bool kernalEmulation);
    /// Power-on state again; with `clearMemory` the RAM is zeroed as well. Symbols, breakpoints and
    /// checkpoints stay.
    void reset(bool clearMemory);

    [[nodiscard]] Cpu& cpu() { return *cpu_; }
    [[nodiscard]] const Cpu& cpu() const { return *cpu_; }
    [[nodiscard]] Memory& memory() { return *memory_; }
    [[nodiscard]] const Memory& memory() const { return *memory_; }
    [[nodiscard]] C64* c64() { return c64_.get(); }
    [[nodiscard]] const C64* c64() const { return c64_.get(); }
    [[nodiscard]] Kernal* kernal() { return kernal_.get(); }
    [[nodiscard]] const Kernal* kernal() const { return kernal_.get(); }

    /// Reads / writes like the processor (C64: banking, I/O registers; writes under ROM reach the RAM).
    [[nodiscard]] Byte peek(Address a) const;
    void poke(Address a, Byte v);
    /// `count` bytes from `start` as the processor sees them, in a memory copy (for the decoder).
    [[nodiscard]] Memory view(Address start, unsigned count) const;

    [[nodiscard]] Snapshot capture() const;
    /// Throws if the snapshot belongs to the other machine type.
    void restore(const Snapshot& snapshot);

    /// Loads a ROM image ("basic", "kernal", "char"); a real KERNAL replaces the emulation (CPU reset).
    void loadRom(const std::string& which, const std::vector<Byte>& image);
    /// Inserts a CRT cartridge (hardware type 0) and resets; see loadFile.
    LoadedFile insertCartridge(const std::string& path);
    /// Files that the emulated KERNAL's LOAD reads: a .prg/.p00 file (answers every name), a directory, or a
    /// D64/T64 image (LOAD"NAME",8 searches its directory; `*` and `?` work as on the C64).
    void attach(const std::string& path);
    [[nodiscard]] const std::string& attached() const { return attached_; }
    /// Loads a program file into RAM and sets the PC to its entry. `type`: auto, prg, p00, raw (raw needs
    /// `address`; for prg/p00 it overrides the load address), d64/t64 (or a path "image.d64:NAME"), crt (inserts the
    /// cartridge and resets; PC = its cold start or reset vector).
    LoadedFile loadFile(const std::string& path, const std::string& type = "auto",
                        std::optional<Address> address = std::nullopt);

    /// Symbol lookup for expressions: user symbols, then (C64) the KERNAL jump-table names.
    bool lookupSymbol(const std::string& name, std::int64_t& value) const;
    /// Address -> name for the disassembler (user symbols win over KERNAL names).
    [[nodiscard]] std::map<Address, std::string> reverseSymbols() const;
    /// Symbols to predefine when assembling (user symbols plus, on the C64, the KERNAL names).
    [[nodiscard]] std::map<std::string, std::int64_t> assemblerSymbols() const;

    void addWatch(const WatchSpec& w);
    void removeWatch(Address start);
    void clearWatches();
    [[nodiscard]] const std::vector<WatchSpec>& watches() const { return watches_; }

    /// Text screen (40x25) as the VIC sees it, trailing spaces removed; empty for the generic machine.
    [[nodiscard]] std::vector<std::string> screenLines() const;
    /// Display mode from $D011/$D016 ("text", "multicolor bitmap", ...).
    [[nodiscard]] std::string screenMode() const;

    std::map<std::string, std::int64_t> symbols;
    /// [start, end) of the program loaded or assembled last (the default range of the analysis).
    std::optional<std::pair<Address, std::uint32_t>> programRange;
    std::map<std::string, Snapshot> checkpoints;

private:
    void applyWatches();

    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<C64> c64_;        // null = generic machine
    std::unique_ptr<Kernal> kernal_;  // null = no KERNAL emulation (generic machine or a real KERNAL ROM)
    std::string attached_;
    std::vector<WatchSpec> watches_;
};

}  // namespace m6510
