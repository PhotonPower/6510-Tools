#include "m6510/snapshot.hpp"

#include <cstring>

#include "m6510/image_io.hpp"

namespace m6510 {

namespace {

constexpr char kMagic[8] = {'M', '6', '5', '1', '0', 'S', 'N', '1'};

class Writer {
public:
    std::vector<Byte> out;
    void u8(unsigned v) { out.push_back(static_cast<Byte>(v)); }
    void u16(unsigned v) {
        u8(v & 0xFF);
        u8((v >> 8) & 0xFF);
    }
    void u32(std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    template <class C>
    void bytes(const C& c) {
        out.insert(out.end(), c.begin(), c.end());
    }
};

class Reader {
public:
    explicit Reader(const std::vector<Byte>& b) : b_(b) {}
    void need(std::size_t n) const {
        if (pos_ + n > b_.size()) throw IoError("snapshot is truncated");
    }
    unsigned u8() {
        need(1);
        return b_[pos_++];
    }
    unsigned u16() {
        const unsigned lo = u8();
        return lo | (u8() << 8);
    }
    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        return lo | (static_cast<std::uint32_t>(u16()) << 16);
    }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        return lo | (static_cast<std::uint64_t>(u32()) << 32);
    }
    template <class C>
    void bytes(C& c) {
        need(c.size());
        std::copy(b_.begin() + static_cast<std::ptrdiff_t>(pos_), b_.begin() + static_cast<std::ptrdiff_t>(pos_ + c.size()), c.begin());
        pos_ += c.size();
    }
    [[nodiscard]] bool done() const { return pos_ == b_.size(); }

private:
    const std::vector<Byte>& b_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<Byte> serialize(const Snapshot& s) {
    Writer w;
    for (char c : kMagic) w.u8(static_cast<Byte>(c));
    w.u8(1);  // version
    const CpuState& c = s.cpu;
    w.u8(c.regs.a);
    w.u8(c.regs.x);
    w.u8(c.regs.y);
    w.u8(c.regs.s);
    w.u8(c.regs.p);
    w.u16(c.regs.pc);
    w.u8((c.halted ? 1 : 0) | (c.irqLine ? 2 : 0) | (c.nmiPending ? 4 : 0) | (c.irqInhibit ? 8 : 0) | (c.portEnabled ? 16 : 0));
    w.u8(c.portDirection);
    w.u8(c.portData);
    w.u64(c.instructions);
    w.u64(c.cycles);
    w.u64(c.interrupts);
    w.u32(static_cast<std::uint32_t>(c.depth));
    w.bytes(s.memory.raw());
    w.u8(s.c64 ? 1 : 0);
    if (s.c64) {
        const C64::State& m = *s.c64;
        w.bytes(m.color);
        w.bytes(m.vic);
        w.bytes(m.sid);
        w.bytes(m.cia1);
        w.bytes(m.cia2);
        w.u8(m.config);
        w.u8(m.vicIrq);
        w.u8(m.cia1Icr);
        w.u8(m.cia1Mask);
        w.u16(m.cia1Latch);
        w.u64(m.cia1Start);
        w.u64(m.nextTimer);
        w.u64(m.lastRasterCheck);
        w.u8(m.cia2Icr);
        w.u8(m.cia2Mask);
        w.u32(m.random);
        w.u8(m.externalIrq ? 1 : 0);
    }
    w.u8(s.kernal ? 1 : 0);
    if (s.kernal) {
        w.u16(static_cast<unsigned>(std::min<std::size_t>(s.kernal->keys.size(), 0xFFFF)));
        for (std::size_t i = 0; i < s.kernal->keys.size() && i < 0xFFFF; ++i) w.u8(s.kernal->keys[i]);
    }
    return w.out;
}

Snapshot deserializeSnapshot(const std::vector<Byte>& bytes) {
    Reader r(bytes);
    for (char c : kMagic)
        if (r.u8() != static_cast<Byte>(c)) throw IoError("not a 6510 snapshot (missing M6510SN1)");
    if (r.u8() != 1) throw IoError("unsupported snapshot version");
    Snapshot s;
    CpuState& c = s.cpu;
    c.regs.a = static_cast<Byte>(r.u8());
    c.regs.x = static_cast<Byte>(r.u8());
    c.regs.y = static_cast<Byte>(r.u8());
    c.regs.s = static_cast<Byte>(r.u8());
    c.regs.p = static_cast<Byte>(r.u8());
    c.regs.pc = static_cast<Address>(r.u16());
    const unsigned f = r.u8();
    if (f & ~31U) throw IoError("bad flags in snapshot");
    c.halted = f & 1;
    c.irqLine = f & 2;
    c.nmiPending = f & 4;
    c.irqInhibit = f & 8;
    c.portEnabled = f & 16;
    c.portDirection = static_cast<Byte>(r.u8());
    c.portData = static_cast<Byte>(r.u8());
    c.instructions = r.u64();
    c.cycles = r.u64();
    c.interrupts = r.u64();
    c.depth = static_cast<int>(r.u32());
    std::vector<Byte> ram(kAddressSpace);
    r.bytes(ram);
    s.memory.load(0, ram);
    const unsigned hasC64 = r.u8();
    if (hasC64 > 1) throw IoError("bad machine flag in snapshot");
    if (hasC64) {
        C64::State m;
        r.bytes(m.color);
        r.bytes(m.vic);
        r.bytes(m.sid);
        r.bytes(m.cia1);
        r.bytes(m.cia2);
        m.config = static_cast<Byte>(r.u8());
        m.vicIrq = static_cast<Byte>(r.u8());
        m.cia1Icr = static_cast<Byte>(r.u8());
        m.cia1Mask = static_cast<Byte>(r.u8());
        m.cia1Latch = static_cast<std::uint16_t>(r.u16());
        m.cia1Start = r.u64();
        m.nextTimer = r.u64();
        m.lastRasterCheck = r.u64();
        m.cia2Icr = static_cast<Byte>(r.u8());
        m.cia2Mask = static_cast<Byte>(r.u8());
        m.random = r.u32();
        const unsigned ext = r.u8();
        if (ext > 1) throw IoError("bad IRQ flag in snapshot");
        m.externalIrq = ext == 1;
        s.c64 = m;
    }
    const unsigned hasKernal = r.u8();
    if (hasKernal > 1) throw IoError("bad KERNAL flag in snapshot");
    if (hasKernal) {
        Kernal::State k;
        const unsigned n = r.u16();
        for (unsigned i = 0; i < n; ++i) k.keys.push_back(static_cast<Byte>(r.u8()));
        s.kernal = k;
    }
    if (!r.done()) throw IoError("trailing data in snapshot");
    return s;
}

std::vector<MemoryChange> diffBytes(const std::vector<Byte>& before, const std::vector<Byte>& after, Address base,
                                    std::size_t gap, std::size_t maxRanges, std::size_t* total) {
    std::vector<MemoryChange> out;
    std::size_t count = 0;
    const std::size_t n = std::min(before.size(), after.size());
    std::size_t i = 0;
    while (i < n) {
        if (before[i] == after[i]) {
            ++i;
            continue;
        }
        std::size_t end = i + 1, last = i;
        while (end < n && end - last <= gap) {
            if (before[end] != after[end]) last = end;
            ++end;
        }
        for (std::size_t k = i; k <= last; ++k) count += before[k] != after[k] ? 1 : 0;
        if (out.size() < maxRanges) {
            MemoryChange c;
            c.address = static_cast<Address>(base + i);
            c.before.assign(before.begin() + static_cast<std::ptrdiff_t>(i), before.begin() + static_cast<std::ptrdiff_t>(last + 1));
            c.after.assign(after.begin() + static_cast<std::ptrdiff_t>(i), after.begin() + static_cast<std::ptrdiff_t>(last + 1));
            out.push_back(std::move(c));
        }
        i = last + 1;
    }
    if (total) *total = count;
    return out;
}

}  // namespace m6510
