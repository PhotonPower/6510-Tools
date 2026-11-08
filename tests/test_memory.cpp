#include "m6510/memory.hpp"

#include <array>

#include "test_util.hpp"

using namespace m6510;

int main() {
    Memory mem;
    CHECK_EQ(mem.read8(0x1234), Byte{0});  // fresh memory reads as 0
    CHECK_EQ(mem.read8(0xFFFF), Byte{0});

    // little-endian composition
    mem.write16(0x1000, 0x1234);
    CHECK_EQ(mem.read8(0x1000), Byte{0x34});
    CHECK_EQ(mem.read8(0x1001), Byte{0x12});
    CHECK_EQ(mem.read16(0x1000), Word{0x1234});
    mem.write8(0x2000, 0xCD);
    mem.write8(0x2001, 0xAB);
    CHECK_EQ(mem.read16(0x2000), Word{0xABCD});

    // 16-bit accesses wrap at 64 KiB
    mem.write16(0xFFFF, 0xBEEF);
    CHECK_EQ(mem.read8(0xFFFF), Byte{0xEF});
    CHECK_EQ(mem.read8(0x0000), Byte{0xBE});
    CHECK_EQ(mem.read16(0xFFFF), Word{0xBEEF});

    // same-page wrap (JMP ($xxFF) bug, zero-page pointer at $FF)
    mem.write8(0x10FF, 0x34);
    mem.write8(0x1000, 0x12);
    mem.write8(0x1100, 0x56);
    CHECK_EQ(mem.read16Wrapped(0x10FF), Word{0x1234});  // high byte from $1000, not $1100
    CHECK_EQ(mem.read16(0x10FF), Word{0x5634});         // the normal read crosses the page
    mem.write8(0x00FF, 0x00);
    mem.write8(0x0000, 0xC0);
    CHECK_EQ(mem.read16Wrapped(0x00FF), Word{0xC000});  // zero page wraps to $00
    mem.write16(0x0080, 0x0801);
    CHECK_EQ(mem.read16Wrapped(0x0080), Word{0x0801});  // no wrap in the middle of a page

    // load / readBytes, including the wrap at the end of memory
    const std::array<Byte, 4> code{0xA9, 0x01, 0x60, 0xEA};  // LDA #$01, RTS, NOP
    mem.load(0xC000, code);
    CHECK(mem.readBytes(0xC000, 4) == std::vector<Byte>(code.begin(), code.end()));
    mem.load(0xFFFE, code);
    CHECK_EQ(mem.read8(0xFFFE), Byte{0xA9});
    CHECK_EQ(mem.read8(0x0001), Byte{0xEA});
    CHECK_EQ(hexBytes(mem.readBytes(0xFFFE, 4)), std::string("A9 01 60 EA"));

    // the memory is copyable (used for checkpoints / snapshots later)
    Memory copy = mem;
    copy.write8(0xC000, 0);
    CHECK_EQ(mem.read8(0xC000), Byte{0xA9});
    CHECK_EQ(copy.read8(0xC000), Byte{0});
    CHECK_EQ(mem.raw()[0xC000], Byte{0xA9});

    mem.clear();
    CHECK_EQ(mem.read16(0x1000), Word{0});
    CHECK_EQ(mem.read8(0xC000), Byte{0});

    // fixed locations
    CHECK_EQ(kResetVector, Address{0xFFFC});
    CHECK_EQ(kStackPage, Address{0x0100});
    CHECK_EQ(kAddressSpace, std::size_t{0x10000});

    return finish("memory");
}
