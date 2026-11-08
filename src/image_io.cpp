#include "m6510/image_io.hpp"

#include "m6510/d64.hpp"
#include "m6510/t64.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

#include "m6510/format.hpp"
#include "m6510/petscii.hpp"

namespace m6510 {

std::vector<Byte> readBinaryFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("cannot open '" + path + "'");
    std::vector<Byte> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw IoError("cannot read '" + path + "'");
    return data;
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("cannot open '" + path + "'");
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeBinaryFile(const std::string& path, const std::vector<Byte>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw IoError("cannot create '" + path + "'");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw IoError("cannot write '" + path + "'");
}

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw IoError("cannot create '" + path + "'");
    out << text;
    if (!out) throw IoError("cannot write '" + path + "'");
}

Program parsePrg(const std::vector<Byte>& file) {
    if (file.size() < 2) throw IoError("PRG file too short (needs a 2-byte load address)");
    Program p;
    p.address = static_cast<Address>(file[0] | (file[1] << 8));
    p.bytes.assign(file.begin() + 2, file.end());
    if (p.end() > 0x10000) {
        throw IoError("PRG data runs past $FFFF (load address " + formatHex(p.address, 4, NumberStyle::Dollar) + ", " +
                      std::to_string(p.bytes.size()) + " bytes)");
    }
    return p;
}

std::vector<Byte> makePrg(Address address, const std::vector<Byte>& bytes) {
    std::vector<Byte> out;
    out.reserve(bytes.size() + 2);
    out.push_back(static_cast<Byte>(address & 0xFF));
    out.push_back(static_cast<Byte>(address >> 8));
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

bool looksLikeP00(const std::vector<Byte>& file) {
    static const char kSig[] = "C64File";
    if (file.size() < 26) return false;
    for (int i = 0; i < 7; ++i)
        if (file[static_cast<std::size_t>(i)] != static_cast<Byte>(kSig[i])) return false;
    return file[7] == 0;
}

Program parseP00(const std::vector<Byte>& file) {
    if (!looksLikeP00(file)) throw IoError("not a P00 file (missing C64File signature)");
    std::string name;
    for (std::size_t i = 8; i < 8 + 17 && file[i] != 0; ++i) name += petsciiToText(file[i]);
    Program p = parsePrg(std::vector<Byte>(file.begin() + 26, file.end()));
    p.name = name;
    return p;
}

std::string fileExtension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

Program loadProgramFile(const std::string& path) {
    const std::vector<Byte> file = readBinaryFile(path);
    if (looksLikeP00(file)) return parseP00(file);
    return parsePrg(file);
}

void loadProgram(Memory& memory, const Program& program) { memory.load(program.address, program.bytes); }

std::optional<std::pair<std::string, std::string>> splitImageSpec(const std::string& spec) {
    auto isImage = [](const std::string& p) {
        const std::string e = fileExtension(p);
        return e == ".d64" || e == ".t64";
    };
    if (isImage(spec)) return std::make_pair(spec, std::string("*"));
    const std::size_t colon = spec.rfind(':');
    if (colon != std::string::npos && colon > 0 && isImage(spec.substr(0, colon))) return std::make_pair(spec.substr(0, colon), spec.substr(colon + 1));
    return std::nullopt;
}

Program loadProgramSpec(const std::string& spec, std::string* description) {
    const auto image = splitImageSpec(spec);
    if (!image) {
        if (description) *description = spec;
        return loadProgramFile(spec);
    }
    const auto& [path, name] = *image;
    if (fileExtension(path) == ".d64") {
        const D64Image disk(readBinaryFile(path));
        const auto entry = disk.find(name.empty() ? "*" : name);
        if (!entry) throw IoError("no file matching \"" + name + "\" on " + path);
        if (entry->type != "PRG") throw IoError("\"" + entry->name + "\" on " + path + " is a " + entry->type + " file, not a program");
        if (description) *description = path + ": \"" + entry->name + "\" (PRG, " + std::to_string(entry->blocks) + " blocks)";
        return disk.readProgram(*entry);
    }
    const T64Image tape(readBinaryFile(path));
    const auto entry = tape.find(name.empty() ? "*" : name);
    if (!entry) throw IoError("no file matching \"" + name + "\" in " + path);
    if (description) *description = path + ": \"" + entry->name + "\" (" + std::to_string(entry->length) + " bytes)";
    return tape.readProgram(*entry);
}

}  // namespace m6510
