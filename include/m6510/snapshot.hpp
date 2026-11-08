#pragma once

#include <optional>
#include <vector>

#include "m6510/c64.hpp"
#include "m6510/cpu.hpp"
#include "m6510/kernal.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

/// The complete machine state: processor, RAM and - for the C64 - chips, colour RAM and the state of
/// the KERNAL emulation. Resuming from a snapshot is bit-identical to continuing the original run.
/// ROM images are not part of it (they are configuration, not state).
struct Snapshot {
    CpuState cpu;
    Memory memory;
    std::optional<C64::State> c64;
    std::optional<Kernal::State> kernal;
};

/// File format "M6510SN1" (docs/formats.md): little-endian fields, the 64 KiB RAM, then the optional
/// parts. Throws IoError on malformed data (every truncation is rejected).
[[nodiscard]] std::vector<Byte> serialize(const Snapshot& snapshot);
[[nodiscard]] Snapshot deserializeSnapshot(const std::vector<Byte>& bytes);

/// A run of changed bytes between two memories.
struct MemoryChange {
    Address address = 0;
    std::vector<Byte> before, after;
};
/// Differences between two byte arrays of the same size (adjacent changes with a gap <= `gap` merged).
/// At most `maxRanges` ranges are returned; `total` (if given) receives the number of changed bytes.
/// `base` is added to the offsets (colour RAM: $D800).
[[nodiscard]] std::vector<MemoryChange> diffBytes(const std::vector<Byte>& before, const std::vector<Byte>& after,
                                                  Address base = 0, std::size_t gap = 8, std::size_t maxRanges = 1000,
                                                  std::size_t* total = nullptr);

}  // namespace m6510
