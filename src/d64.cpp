#include "m6510/d64.hpp"

#include <cctype>
#include <set>

#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

constexpr std::size_t kSize35 = 174848, kSize35Err = 175531, kSize40 = 196608, kSize40Err = 197376;

std::string trackSector(unsigned t, unsigned s) { return std::to_string(t) + "/" + std::to_string(s); }

// PETSCII name -> text: printable characters as they are, the rest like petsciiToText ({$xx}); padding removed.
std::string nameText(const std::vector<Byte>& raw) {
    std::size_t n = raw.size();
    while (n > 0 && (raw[n - 1] == 0xA0)) --n;
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += petsciiToText(raw[i]);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool cbmNameMatches(const std::string& pattern, const std::string& name) {
    const std::string p = lower(pattern), n = lower(name);
    for (std::size_t i = 0;; ++i) {
        if (i < p.size() && p[i] == '*') return true;
        if (i == p.size() || i == n.size()) return i == p.size() && i == n.size();
        if (p[i] != '?' && p[i] != n[i]) return false;
    }
}

unsigned D64Image::sectorsPerTrack(unsigned track) {
    if (track >= 1 && track <= 17) return 21;
    if (track >= 18 && track <= 24) return 19;
    if (track >= 25 && track <= 30) return 18;
    if (track >= 31 && track <= 40) return 17;
    return 0;
}

D64Image::D64Image(std::vector<Byte> bytes) : data_(std::move(bytes)) {
    switch (data_.size()) {
        case kSize35: tracks_ = 35; break;
        case kSize35Err: tracks_ = 35; hasErrors_ = true; break;
        case kSize40: tracks_ = 40; break;
        case kSize40Err: tracks_ = 40; hasErrors_ = true; break;
        default:
            throw IoError("not a D64 image: " + std::to_string(data_.size()) + " bytes (expected 174848 or 196608, optionally with error bytes)");
    }
}

bool D64Image::validBlock(unsigned track, unsigned sector) const {
    return track >= 1 && track <= tracks_ && sector < sectorsPerTrack(track);
}

std::size_t D64Image::offset(unsigned track, unsigned sector) const {
    std::size_t n = 0;
    for (unsigned t = 1; t < track; ++t) n += sectorsPerTrack(t);
    return (n + sector) * 256;
}

std::vector<Byte> D64Image::block(unsigned track, unsigned sector) const {
    if (!validBlock(track, sector)) throw IoError("track/sector " + trackSector(track, sector) + " is not on the disk");
    const std::size_t o = offset(track, sector);
    return std::vector<Byte>(data_.begin() + static_cast<std::ptrdiff_t>(o), data_.begin() + static_cast<std::ptrdiff_t>(o + 256));
}

std::optional<Byte> D64Image::errorCode(unsigned track, unsigned sector) const {
    if (!hasErrors_ || !validBlock(track, sector)) return std::nullopt;
    const std::size_t sectors = tracks_ == 35 ? 683 : 768;
    return data_[sectors * 256 + offset(track, sector) / 256];
}

unsigned D64Image::badSectors() const {
    if (!hasErrors_) return 0;
    const std::size_t sectors = tracks_ == 35 ? 683 : 768;
    unsigned n = 0;
    for (std::size_t i = 0; i < sectors; ++i) {
        const Byte e = data_[sectors * 256 + i];
        if (e != 0 && e != 1) ++n;
    }
    return n;
}

std::string D64Image::diskName() const {
    const std::vector<Byte> bam = block(18, 0);
    return nameText(std::vector<Byte>(bam.begin() + 0x90, bam.begin() + 0xA0));
}

std::string D64Image::diskId() const {
    const std::vector<Byte> bam = block(18, 0);
    return nameText({bam[0xA2], bam[0xA3]});
}

std::string D64Image::dosType() const {
    const std::vector<Byte> bam = block(18, 0);
    return nameText({bam[0xA5], bam[0xA6]});
}

unsigned D64Image::freeBlocks() const {
    const std::vector<Byte> bam = block(18, 0);
    unsigned n = 0;
    for (unsigned t = 1; t <= 35; ++t)
        if (t != 18) n += bam[4 * t];
    return n;
}

std::vector<D64Entry> D64Image::directory() const {
    std::vector<D64Entry> out;
    const std::vector<Byte> bam = block(18, 0);
    unsigned t = bam[0], s = bam[1];
    if (t != 18) {
        problems_.push_back("the BAM points to the directory at " + trackSector(t, s) + " instead of track 18");
    }
    std::set<std::pair<unsigned, unsigned>> seen;
    unsigned index = 0;
    while (t != 0) {
        if (!validBlock(t, s)) {
            problems_.push_back("directory chain leaves the disk at " + trackSector(t, s));
            break;
        }
        if (!seen.insert({t, s}).second) {
            problems_.push_back("directory chain loops at " + trackSector(t, s));
            break;
        }
        const std::vector<Byte> b = block(t, s);
        for (unsigned e = 0; e < 8; ++e) {
            const Byte* p = b.data() + e * 32;
            const Byte typeByte = p[2];
            if (typeByte == 0) continue;
            D64Entry entry;
            static const char* types[] = {"DEL", "SEQ", "PRG", "USR", "REL"};
            entry.type = (typeByte & 7) < 5 ? types[typeByte & 7] : "???";
            entry.closed = (typeByte & 0x80) != 0;
            entry.locked = (typeByte & 0x40) != 0;
            entry.track = p[3];
            entry.sector = p[4];
            entry.rawName.assign(p + 5, p + 21);
            entry.name = nameText(entry.rawName);
            entry.blocks = static_cast<unsigned>(p[30] | (p[31] << 8));
            entry.index = index++;
            out.push_back(std::move(entry));
        }
        t = b[0];
        s = b[1];
    }
    return out;
}

std::optional<D64Entry> D64Image::find(const std::string& pattern) const {
    for (const D64Entry& e : directory()) {
        if (e.type == "DEL" && !e.closed) continue;
        if (pattern == "*" || cbmNameMatches(pattern, e.name)) return e;
    }
    return std::nullopt;
}

std::vector<Byte> D64Image::readFile(const D64Entry& entry) const {
    std::vector<Byte> out;
    unsigned t = entry.track, s = entry.sector, blocks = 0;
    std::set<std::pair<unsigned, unsigned>> seen;
    while (true) {
        if (!validBlock(t, s)) throw IoError("the file \"" + entry.name + "\" leaves the disk at " + trackSector(t, s));
        if (!seen.insert({t, s}).second) throw IoError("the file \"" + entry.name + "\" loops at " + trackSector(t, s));
        const std::vector<Byte> b = block(t, s);
        ++blocks;
        if (b[0] == 0) {
            if (b[1] < 1) problems_.push_back("the last block of \"" + entry.name + "\" has an end position below 2");
            const unsigned last = b[1];
            for (unsigned i = 2; i <= last && i < 256; ++i) out.push_back(b[i]);
            break;
        }
        out.insert(out.end(), b.begin() + 2, b.end());
        t = b[0];
        s = b[1];
    }
    if (blocks != entry.blocks)
        problems_.push_back("\"" + entry.name + "\": the directory says " + std::to_string(entry.blocks) + " blocks, the chain has " +
                            std::to_string(blocks));
    return out;
}

Program D64Image::readProgram(const D64Entry& entry) const {
    const std::vector<Byte> bytes = readFile(entry);
    if (bytes.size() < 2) throw IoError("the file \"" + entry.name + "\" is too short for a program (no load address)");
    return parsePrg(bytes);
}

}  // namespace m6510
