#pragma once

#include <array>
#include <span>
#include <vector>

#include "m6510/types.hpp"

namespace m6510 {

/// Flat 64 KiB RAM, little-endian - the raw storage behind the 6510's address space.
///
/// - Addresses wrap at 64 KiB like the hardware's 16-bit address bus.
/// - Multi-byte accesses compose bytes, least significant byte first.
/// - `Memory` is plain RAM. The processor port at $00/$01, the C64's ROM/I/O banking and the
///   page-wrap quirks of individual addressing modes are the job of the CPU and machine layers,
///   so that tools such as the disassembler can read freely. `read16Wrapped` is provided as the
///   shared building block for those quirks.
class Memory {
public:
    Memory() { ram_.fill(0); }

    [[nodiscard]] Byte read8(Address address) const { return ram_[address]; }
    void write8(Address address, Byte value) { ram_[address] = value; }

    /// Little-endian word; the high byte is read from `address + 1` (wrapping at $FFFF).
    [[nodiscard]] Word read16(Address address) const {
        return static_cast<Word>(read8(address) | (read8(static_cast<Address>(address + 1)) << 8));
    }
    void write16(Address address, Word value) {
        write8(address, static_cast<Byte>(value));
        write8(static_cast<Address>(address + 1), static_cast<Byte>(value >> 8));
    }

    /// Little-endian word whose high byte comes from the *same page*: at $xxFF the high byte is
    /// read from $xx00. This is how the NMOS 6502 fetches the pointer of `JMP ($xxFF)` and of
    /// `(zp,X)` / `(zp),Y` at $FF (zero page wraps to $00).
    [[nodiscard]] Word read16Wrapped(Address address) const {
        const auto high = static_cast<Address>((address & 0xFF00U) | ((address + 1U) & 0x00FFU));
        return static_cast<Word>(read8(address) | (read8(high) << 8));
    }

    /// Copies `bytes` to `address`; the address wraps at 64 KiB like the hardware does.
    void load(Address address, std::span<const Byte> bytes) {
        for (Byte b : bytes) write8(address++, b);
    }

    /// Reads `count` bytes starting at `address` (wrapping at 64 KiB).
    [[nodiscard]] std::vector<Byte> readBytes(Address address, std::size_t count) const {
        std::vector<Byte> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.push_back(read8(address++));
        return out;
    }

    void clear() { ram_.fill(0); }

    /// Direct access to the whole RAM (snapshots, diffs, video rendering).
    [[nodiscard]] const std::array<Byte, kAddressSpace>& raw() const { return ram_; }

private:
    std::array<Byte, kAddressSpace> ram_{};
};

}  // namespace m6510
