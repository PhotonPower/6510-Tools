#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "m6510/assembler.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

struct IoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Reads/writes whole files. Throw IoError.
std::vector<Byte> readBinaryFile(const std::string& path);
std::string readTextFile(const std::string& path);
void writeBinaryFile(const std::string& path, const std::vector<Byte>& bytes);
void writeTextFile(const std::string& path, const std::string& text);

/// A program image: load address and bytes (what a PRG file holds).
struct Program {
    Address address = 0;
    std::vector<Byte> bytes;
    std::string name;  ///< file name inside a P00/T64/D64 (PETSCII as text), if known

    [[nodiscard]] std::uint32_t end() const { return address + static_cast<std::uint32_t>(bytes.size()); }
};

/// Parses a PRG file: two bytes load address (little-endian), then the data. Throws IoError if the
/// file is shorter than 2 bytes or the data would run past $FFFF.
Program parsePrg(const std::vector<Byte>& file);
/// Builds a PRG file.
std::vector<Byte> makePrg(Address address, const std::vector<Byte>& bytes);

/// Parses a PC64 file (.P00): "C64File\0", 17 bytes name, 1 byte record size, then PRG data.
Program parseP00(const std::vector<Byte>& file);
bool looksLikeP00(const std::vector<Byte>& file);

/// Loads a program file by its contents/extension: P00 (signature), else PRG.
Program loadProgramFile(const std::string& path);

/// Copies the program into memory at its load address.
void loadProgram(Memory& memory, const Program& program);

/// Lower-case file extension including the dot (".prg"), or "".
std::string fileExtension(const std::string& path);

}  // namespace m6510
