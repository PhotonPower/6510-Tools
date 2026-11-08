#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "m6510/assembler.hpp"  // Diagnostic
#include "m6510/decoder.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

struct DisassemblerOptions {
    NumberStyle style = NumberStyle::Dollar;
    bool showBytes = true;   ///< listing: show the instruction bytes
    bool asmSource = false;  ///< re-assemblable source (`* =`, labels, no addresses/bytes)
    bool xrefs = true;       ///< listing: show who jumps/calls/references a label
};

/// Disassembles the address range [start, end) of a memory image (end may be $10000).
///
/// Two modes (like the sister projects): `analyzeFlow()` follows JSR/JMP/branches from the entry
/// points and separates code from data; `analyzeLinear()` decodes sequentially. Bytes that are not
/// code become `.BYTE` lines (printable runs as strings). Targets inside the range get labels
/// (`L_XXXX` or a user symbol); a target in the middle of an instruction (self-modifying code)
/// is written `label+n`. A BASIC starter at the start of the range (`10 SYS2061`) becomes `.BASIC`,
/// and its SYS address is the default entry point.
class Disassembler {
public:
    Disassembler(const Memory& memory, Address start, std::uint32_t end, CpuLevel level = CpuLevel::Stable);

    void addEntry(Address address);                            ///< starting point for analyzeFlow()
    void addSymbol(Address address, const std::string& name);  ///< user or system name (also outside the range)
    void addComment(Address address, const std::string& text);

    void analyzeFlow();
    void analyzeLinear();

    [[nodiscard]] std::size_t codeBytes() const;
    [[nodiscard]] const std::set<Address>& entries() const { return entries_; }

    [[nodiscard]] std::string render(const DisassemblerOptions& options = {}) const;

private:
    enum : std::uint8_t { kData = 0, kStart = 1, kCont = 2, kBasic = 3 };
    [[nodiscard]] bool inRange(std::uint32_t a) const { return a >= start_ && a < end_; }
    [[nodiscard]] std::uint8_t state(std::uint32_t a) const { return state_[a - start_]; }
    [[nodiscard]] Address lineStart(Address a) const;  ///< start of the instruction containing `a`
    [[nodiscard]] std::string labelName(Address a) const;
    [[nodiscard]] std::string symbolFor(Address a) const;
    [[nodiscard]] Instruction decodeAt(Address a, NumberStyle style) const;
    void detectBasic();
    bool mark(const Instruction& in);
    void noteReferences(const Instruction& in, std::vector<Address>* work);
    void finishLabels();

    const Memory& memory_;
    std::uint32_t start_, end_;
    CpuLevel level_;
    std::vector<std::uint8_t> state_;
    std::set<Address> entries_;
    std::set<Address> labels_;
    std::map<Address, std::string> symbols_;
    std::map<Address, std::string> comments_;
    std::map<Address, std::set<Address>> xrefs_;  // target -> addresses of instructions referring to it
    std::size_t basicLength_ = 0;
    unsigned basicLine_ = 0;
    Address basicTarget_ = 0;
};

/// Loads a comment file (lines `address,"text"`, ';' comments) into `dis`: every commented address
/// becomes an entry point and a comment. Returns diagnostics for malformed lines.
std::vector<Diagnostic> loadCommentFile(const std::string& text, Disassembler& dis);

/// Parses a symbol file: the assembler's `-s` format (`name = $XXXX`) or VICE labels (`al C:XXXX .name`).
std::vector<Diagnostic> parseSymbolFile(const std::string& text, std::map<Address, std::string>& out);

/// Names of the KERNAL jump table ($FF81-$FFF3) and the hardware vectors, for disassembly.
const std::map<Address, std::string>& kernalSymbols();

}  // namespace m6510
