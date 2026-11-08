#include "m6510/t64.hpp"

#include <algorithm>

#include "m6510/d64.hpp"
#include "m6510/format.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

std::string nameText(const Byte* p, std::size_t n) {
    while (n > 0 && (p[n - 1] == 0x20 || p[n - 1] == 0xA0 || p[n - 1] == 0)) --n;
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += petsciiToText(p[i]);
    return out;
}

unsigned le16(const std::vector<Byte>& d, std::size_t o) { return static_cast<unsigned>(d[o] | (d[o + 1] << 8)); }
std::uint32_t le32(const std::vector<Byte>& d, std::size_t o) {
    return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) | (static_cast<std::uint32_t>(d[o + 2]) << 16) |
           (static_cast<std::uint32_t>(d[o + 3]) << 24);
}

}  // namespace

T64Image::T64Image(std::vector<Byte> bytes) : data_(std::move(bytes)) {
    if (data_.size() < 64 || data_[0] != 'C' || data_[1] != '6' || data_[2] != '4')
        throw IoError("not a T64 archive (no \"C64\" signature)");
    version_ = le16(data_, 0x20);
    const unsigned maxEntries = le16(data_, 0x22);
    unsigned used = le16(data_, 0x24);
    name_ = nameText(data_.data() + 0x28, 24);
    if (maxEntries == 0) throw IoError("T64: the directory has no room for entries");
    if (64 + static_cast<std::size_t>(maxEntries) * 32 > data_.size()) throw IoError("T64: the directory is larger than the file");
    if (used == 0) problems_.push_back("the header says 0 used entries; the directory is read anyway");
    std::vector<T64Entry> all;
    for (unsigned i = 0; i < maxEntries; ++i) {
        const std::size_t o = 64 + static_cast<std::size_t>(i) * 32;
        T64Entry e;
        e.entryType = data_[o];
        if (e.entryType == 0) continue;
        e.fileType = data_[o + 1];
        e.start = static_cast<Address>(le16(data_, o + 2));
        e.end = static_cast<Address>(le16(data_, o + 4));
        e.offset = le32(data_, o + 8);
        e.name = nameText(data_.data() + o + 16, 16);
        e.index = i;
        if (e.offset >= data_.size()) {
            problems_.push_back("entry " + std::to_string(i) + " (\"" + e.name + "\"): the data offset lies behind the end of the file");
            continue;
        }
        all.push_back(e);
    }
    // the real length: up to the next entry's data or the end of the file
    for (T64Entry& e : all) {
        std::uint32_t limit = static_cast<std::uint32_t>(data_.size());
        for (const T64Entry& x : all)
            if (x.offset > e.offset) limit = std::min(limit, x.offset);
        const std::uint32_t available = limit - e.offset;
        const std::uint32_t stored = e.end > e.start ? static_cast<std::uint32_t>(e.end - e.start) : (e.end == 0 ? 0x10000u - e.start : 0u);
        e.length = stored;
        if (stored == 0 || stored > available) {
            e.length = std::min<std::uint32_t>(available, 0x10000u - e.start);
            problems_.push_back("\"" + e.name + "\": the end address $" + hexDigits(e.end, 4) + " does not fit the data; using " +
                                std::to_string(e.length) + " bytes");
        }
    }
    if (used != 0 && used != all.size())
        problems_.push_back("the header says " + std::to_string(used) + " used entries, the directory has " + std::to_string(all.size()));
    entries_ = std::move(all);
}

std::optional<T64Entry> T64Image::find(const std::string& pattern) const {
    for (const T64Entry& e : entries_)
        if (pattern == "*" || cbmNameMatches(pattern, e.name)) return e;
    return std::nullopt;
}

Program T64Image::readProgram(const T64Entry& entry) const {
    Program p;
    p.address = entry.start;
    p.bytes.assign(data_.begin() + entry.offset, data_.begin() + entry.offset + entry.length);
    return p;
}

}  // namespace m6510
