#pragma once

#include <cstddef>
#include <cstdint>

namespace m6510 {

using Byte = std::uint8_t;   ///< 8 bit: registers A, X, Y, S, P and every memory cell
using Word = std::uint16_t;  ///< 16 bit: PC, addresses, little-endian words in memory

/// The 6510 drives A0-A15: a flat 64 KiB address space. What appears at an address on a C64
/// (RAM, ROM, I/O) depends on the processor port ($01) and is handled by the machine layer,
/// not by `Memory` (see docs/c64.md).
using Address = std::uint16_t;

inline constexpr unsigned kAddressBits = 16;
inline constexpr std::size_t kAddressSpace = std::size_t{1} << kAddressBits;  // 64 KiB

/// Fixed locations of the 6502 family.
inline constexpr Address kZeroPage = 0x0000;     ///< $0000-$00FF, short addressing modes
inline constexpr Address kStackPage = 0x0100;    ///< $0100-$01FF, S is the offset in this page
inline constexpr Address kNmiVector = 0xFFFA;    ///< NMI handler address (little-endian)
inline constexpr Address kResetVector = 0xFFFC;  ///< RESET handler address
inline constexpr Address kIrqVector = 0xFFFE;    ///< IRQ and BRK handler address

/// The 6510's on-chip I/O port (not present on a plain 6502).
inline constexpr Address kPortDirection = 0x0000;  ///< data direction register (1 = output)
inline constexpr Address kPortData = 0x0001;       ///< data register; on the C64 bits 0-2 select the memory configuration

/// Status register bits (P). Bit 5 has no flip-flop and always reads as 1; B exists only in the
/// copy of P that BRK/PHP push onto the stack.
enum Flag : Byte {
    kFlagC = 0x01,  ///< carry
    kFlagZ = 0x02,  ///< zero
    kFlagI = 0x04,  ///< IRQ disable
    kFlagD = 0x08,  ///< decimal mode
    kFlagB = 0x10,  ///< break (pushed copy only)
    kFlagU = 0x20,  ///< unused, always 1
    kFlagV = 0x40,  ///< overflow
    kFlagN = 0x80   ///< negative
};

}  // namespace m6510
