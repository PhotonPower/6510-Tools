#pragma once

// Builds D64, T64 and CRT images for the tests (no images of other people's programs in the repository).

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "m6510/d64.hpp"
#include "m6510/types.hpp"

namespace testimg {

using m6510::Byte;

class D64Builder {
public:
    explicit D64Builder(unsigned tracks = 35, bool errorBytes = false, std::string name = "TEST DISK", std::string id = "AB")
        : tracks_(tracks), errorBytes_(errorBytes), name_(std::move(name)), id_(std::move(id)) {
        sectors_ = 0;
        for (unsigned t = 1; t <= tracks_; ++t) sectors_ += m6510::D64Image::sectorsPerTrack(t);
        img_.assign(sectors_ * 256 + (errorBytes_ ? sectors_ : 0), 0);
        if (errorBytes_)
            for (unsigned i = 0; i < sectors_; ++i) img_[sectors_ * 256 + i] = 1;
        used_.assign(sectors_, false);
        use(18, 0);
    }

    std::size_t offset(unsigned t, unsigned s) const {
        std::size_t n = 0;
        for (unsigned x = 1; x < t; ++x) n += m6510::D64Image::sectorsPerTrack(x);
        return (n + s) * 256;
    }
    Byte* block(unsigned t, unsigned s) { return img_.data() + offset(t, s); }
    void setError(unsigned t, unsigned s, Byte code) { img_[sectors_ * 256 + offset(t, s) / 256] = code; }

    /// Writes a file and adds a directory entry; returns its first track/sector.
    std::pair<unsigned, unsigned> addFile(const std::string& name, const std::vector<Byte>& data, Byte type = 0x82) {
        std::vector<std::pair<unsigned, unsigned>> chain;
        const std::size_t blocks = data.empty() ? 1 : (data.size() + 253) / 254;
        for (std::size_t i = 0; i < blocks; ++i) chain.push_back(allocate());
        for (std::size_t i = 0; i < blocks; ++i) {
            Byte* b = block(chain[i].first, chain[i].second);
            const std::size_t from = i * 254, n = std::min<std::size_t>(254, data.size() - std::min(data.size(), from));
            if (i + 1 < blocks) {
                b[0] = static_cast<Byte>(chain[i + 1].first);
                b[1] = static_cast<Byte>(chain[i + 1].second);
            } else {
                b[0] = 0;
                b[1] = static_cast<Byte>(n + 1);
            }
            if (n) std::memcpy(b + 2, data.data() + from, n);
        }
        entries_.push_back({name, type, chain[0], static_cast<unsigned>(blocks)});
        return chain[0];
    }

    /// The finished image (BAM and directory written).
    std::vector<Byte> build() {
        // directory sectors on track 18 from sector 1
        const std::size_t dirSectors = std::max<std::size_t>(1, (entries_.size() + 7) / 8);
        for (std::size_t d = 0; d < dirSectors; ++d) {
            const unsigned s = static_cast<unsigned>(1 + d);
            use(18, s);
            Byte* b = block(18, s);
            std::memset(b, 0, 256);
            if (d + 1 < dirSectors) {
                b[0] = 18;
                b[1] = static_cast<Byte>(s + 1);
            } else {
                b[0] = 0;
                b[1] = 0xFF;
            }
            for (std::size_t e = 0; e < 8 && d * 8 + e < entries_.size(); ++e) {
                const Entry& x = entries_[d * 8 + e];
                Byte* p = b + e * 32;
                p[2] = x.type;
                p[3] = static_cast<Byte>(x.first.first);
                p[4] = static_cast<Byte>(x.first.second);
                for (unsigned i = 0; i < 16; ++i) p[5 + i] = i < x.name.size() ? static_cast<Byte>(x.name[i]) : 0xA0;
                p[30] = static_cast<Byte>(x.blocks & 0xFF);
                p[31] = static_cast<Byte>(x.blocks >> 8);
            }
        }
        Byte* bam = block(18, 0);
        bam[0] = 18;
        bam[1] = 1;
        bam[2] = 'A';
        for (unsigned t = 1; t <= 35; ++t) {
            unsigned free = 0;
            for (unsigned s = 0; s < m6510::D64Image::sectorsPerTrack(t); ++s) free += used_[offset(t, s) / 256] ? 0 : 1;
            bam[4 * t] = static_cast<Byte>(free);
        }
        for (unsigned i = 0; i < 16; ++i) bam[0x90 + i] = i < name_.size() ? static_cast<Byte>(name_[i]) : 0xA0;
        bam[0xA0] = bam[0xA1] = 0xA0;
        bam[0xA2] = static_cast<Byte>(id_[0]);
        bam[0xA3] = static_cast<Byte>(id_[1]);
        bam[0xA4] = 0xA0;
        bam[0xA5] = '2';
        bam[0xA6] = 'A';
        return img_;
    }

private:
    struct Entry {
        std::string name;
        Byte type;
        std::pair<unsigned, unsigned> first;
        unsigned blocks;
    };
    void use(unsigned t, unsigned s) { used_[offset(t, s) / 256] = true; }
    std::pair<unsigned, unsigned> allocate() {
        for (unsigned t = 1; t <= tracks_; ++t) {
            if (t == 18) continue;
            for (unsigned s = 0; s < m6510::D64Image::sectorsPerTrack(t); ++s) {
                if (!used_[offset(t, s) / 256]) {
                    use(t, s);
                    return {t, s};
                }
            }
        }
        return {0, 0};
    }

    unsigned tracks_;
    bool errorBytes_;
    std::string name_, id_;
    unsigned sectors_;
    std::vector<Byte> img_;
    std::vector<bool> used_;
    std::vector<Entry> entries_;
};

struct T64File {
    std::string name;
    m6510::Address start;
    std::vector<Byte> data;
    int endOverride = -1;  // write this end address instead of the right one (the common fault)
};

inline std::vector<Byte> buildT64(const std::vector<T64File>& files, const std::string& tapeName = "TEST TAPE", int usedOverride = -1,
                                  unsigned slots = 0) {
    const unsigned max = std::max<unsigned>(slots, static_cast<unsigned>(files.size()));
    std::vector<Byte> out(64 + 32 * max, 0);
    const char* sig = "C64S tape image file";
    std::memcpy(out.data(), sig, std::strlen(sig));
    out[0x20] = 0x00;
    out[0x21] = 0x01;
    out[0x22] = static_cast<Byte>(max);
    const unsigned used = usedOverride >= 0 ? static_cast<unsigned>(usedOverride) : static_cast<unsigned>(files.size());
    out[0x24] = static_cast<Byte>(used);
    for (unsigned i = 0; i < 24; ++i) out[0x28 + i] = i < tapeName.size() ? static_cast<Byte>(tapeName[i]) : 0x20;
    for (std::size_t f = 0; f < files.size(); ++f) {
        const std::size_t o = 64 + 32 * f;
        const auto offset = static_cast<std::uint32_t>(out.size());
        const unsigned end = files[f].endOverride >= 0 ? static_cast<unsigned>(files[f].endOverride)
                                                       : static_cast<unsigned>(files[f].start + files[f].data.size()) & 0xFFFF;
        out[o] = 1;
        out[o + 1] = 0x82;
        out[o + 2] = static_cast<Byte>(files[f].start);
        out[o + 3] = static_cast<Byte>(files[f].start >> 8);
        out[o + 4] = static_cast<Byte>(end);
        out[o + 5] = static_cast<Byte>(end >> 8);
        for (int k = 0; k < 4; ++k) out[o + 8 + static_cast<std::size_t>(k)] = static_cast<Byte>(offset >> (8 * k));
        for (unsigned i = 0; i < 16; ++i) out[o + 16 + i] = i < files[f].name.size() ? static_cast<Byte>(files[f].name[i]) : 0x20;
        out.insert(out.end(), files[f].data.begin(), files[f].data.end());
    }
    return out;
}

struct CrtChipSpec {
    unsigned bank;
    m6510::Address load;
    std::vector<Byte> data;
};

inline std::vector<Byte> buildCrt(unsigned type, bool exrom, bool game, const std::vector<CrtChipSpec>& chips, const std::string& name = "TEST CART") {
    std::vector<Byte> out(0x40, 0);
    std::memcpy(out.data(), "C64 CARTRIDGE   ", 16);
    out[0x13] = 0x40;
    out[0x14] = 1;
    out[0x16] = static_cast<Byte>(type >> 8);
    out[0x17] = static_cast<Byte>(type);
    out[0x18] = exrom ? 1 : 0;
    out[0x19] = game ? 1 : 0;
    for (std::size_t i = 0; i < name.size() && i < 32; ++i) out[0x20 + i] = static_cast<Byte>(name[i]);
    for (const CrtChipSpec& c : chips) {
        const auto len = static_cast<std::uint32_t>(16 + c.data.size());
        const Byte head[16] = {'C', 'H', 'I', 'P', static_cast<Byte>(len >> 24), static_cast<Byte>(len >> 16), static_cast<Byte>(len >> 8),
                               static_cast<Byte>(len), 0, 0, static_cast<Byte>(c.bank >> 8), static_cast<Byte>(c.bank),
                               static_cast<Byte>(c.load >> 8), static_cast<Byte>(c.load), static_cast<Byte>(c.data.size() >> 8),
                               static_cast<Byte>(c.data.size())};
        out.insert(out.end(), head, head + 16);
        out.insert(out.end(), c.data.begin(), c.data.end());
    }
    return out;
}

}  // namespace testimg
