#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "m6510/instruction.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

/// A table of code addresses reached through an indirect jump (docs/reverse-engineering.md, "Sprungtabellen").
struct JumpTable {
    enum class Kind {
        Vector,   ///< `JMP (vec)` with a vector inside the program that no instruction writes
        Rts,      ///< `LDA hi,X / PHA / LDA lo,X / PHA / RTS`: entries are address - 1
        Lohi,     ///< `LDA lo,X / STA ptr / LDA hi,X / STA ptr+1 / JMP (ptr)`
        Pointers  ///< a row of 16-bit code addresses (from an annotation, or a suggestion found by scanning)
    };
    Kind kind = Kind::Vector;
    Address jump = 0;          ///< the JMP or RTS instruction (0 if the table was not found through one)
    Address table = 0;         ///< the vector, the word table or the table of low bytes
    Address hiTable = 0;       ///< split tables: the table of high bytes
    bool split = false;        ///< separate tables of low and high bytes
    bool minusOne = false;     ///< entries are target - 1 (RTS trick)
    unsigned bytes = 0;        ///< bytes of table data (both tables together)
    bool followed = true;      ///< the targets were analysed as code (false: only a suggestion)
    std::vector<Address> slots;    ///< address of each entry (its low byte)
    std::vector<Address> targets;  ///< code address each entry leads to
};

/// A guess about the type of bytes that are neither code nor annotated.
struct DataGuess {
    enum class Kind { Text, Screen, Zeros, Words, Pointers, Sprites, Charset, Basic };
    Kind kind = Kind::Text;
    Address start = 0;
    unsigned length = 0;
    std::string reason;   ///< why this was guessed
    std::string preview;  ///< Text/Screen/Basic: the first characters
};
[[nodiscard]] const char* toString(DataGuess::Kind kind);

struct RoutineInfo {
    Address entry = 0;
    Address start = 0, end = 0;  ///< hull: lowest address and the address after the highest instruction
    unsigned instructions = 0;
    unsigned bytes = 0;
    std::vector<std::pair<Address, Address>> blocks;  ///< the routine's own code as contiguous blocks [start, end)
    std::vector<Address> callers;        ///< instructions that call it (JSR, or JMP from another routine)
    std::vector<Address> callees;        ///< entries of the routines it calls or jumps to (inside the analysed code)
    std::vector<Address> externalCalls;  ///< JSR/JMP targets outside the analysed range
    std::vector<std::pair<Address, Address>> kernalCalls;  ///< (call site, KERNAL address) for named KERNAL entries
    std::set<Address> ioReads, ioWrites; ///< chip registers ($D000-$DFFF) read / written
    std::set<Address> zeroPage;          ///< zero-page addresses used (pointer pairs with both bytes)
    std::vector<std::pair<Address, int>> portWrites;  ///< writes to $00/$01: (instruction, value or -1 if unknown)
    bool viaTable = false;               ///< reached through a jump table
    int depth = 0;                       ///< 0 = calls no other routine; else 1 + the deepest callee
};

/// Code flow analysis over an address range [from, to) of a memory image (the RAM view of the program):
/// which bytes are code, where the routines are, who calls whom, which jump tables and data there are.
/// Like the disassembler's analysis, but keeps the instructions and the call graph and recognises jump
/// tables (`JMP (vector)`, the RTS trick, split low/high tables), pointer constants in immediate pairs
/// (`LDA #<text / LDY #>text`) and self-modifying code.
class Analysis {
public:
    Analysis(const Memory& view, Address from, std::uint32_t to);

    /// Declares a table of 16-bit code addresses (an annotated `pointers`/`rts_pointers` region).
    void addPointerTable(Address table, unsigned bytes, bool minusOne);
    /// Declares split tables of `count` low and high bytes (an annotated `lohi` region).
    void addLohiTable(Address lo, Address hi, unsigned count, bool minusOne);
    /// Addresses for which `isData` returns true are never decoded as code (annotated data regions).
    void setDataBarrier(std::function<bool(Address)> isData) { isData_ = std::move(isData); }

    void addEntry(Address address);
    /// An address known to be the start of an instruction (e.g. executed in the emulator): followed like an entry, but not a
    /// routine of its own. After the flow, code that only such hints reach gets a routine at the start of each block.
    void addCodeHint(Address address);
    void analyzeFlow();                                ///< follow the flow from all entry points (and found tables)
    void analyzeLinear(Address from, std::uint32_t to);  ///< decode sequentially

    [[nodiscard]] Address from() const { return from_; }
    [[nodiscard]] std::uint32_t to() const { return to_; }
    [[nodiscard]] bool inRange(std::uint32_t a) const { return a >= from_ && a < to_; }
    [[nodiscard]] const Instruction* at(Address a) const;
    /// True if the byte belongs to an analysed instruction (start or operand).
    [[nodiscard]] bool isCode(Address a) const;
    /// Start of the instruction that contains `a` (nullopt if `a` is not code).
    [[nodiscard]] std::optional<Address> instructionStart(Address a) const;
    /// True for the bytes of a jump table found by the analysis.
    [[nodiscard]] bool isTableData(Address a) const;
    [[nodiscard]] const std::map<Address, Instruction>& code() const { return code_; }
    [[nodiscard]] std::size_t codeBytes() const;
    [[nodiscard]] const std::set<Address>& entries() const { return entries_; }
    [[nodiscard]] const std::vector<JumpTable>& jumpTables() const { return tables_; }
    [[nodiscard]] const std::set<Address>& callTargets() const { return callTargets_; }
    [[nodiscard]] const std::set<Address>& jumpTargets() const { return jumpTargets_; }
    /// Addresses referenced as data (memory operands and pointer constants), inside and outside the range.
    [[nodiscard]] const std::set<Address>& dataReferences() const { return refs_; }
    /// Instructions with an immediate that is half of a pointer: instruction -> (pointer, true = low byte).
    [[nodiscard]] const std::map<Address, std::pair<Address, bool>>& pointerImmediates() const { return immediates_; }
    /// Interrupt handlers the program installs (`LDA #<irq / STA $0314 / LDA #>irq / STA $0315`): handler -> vector.
    [[nodiscard]] const std::map<Address, Address>& vectorEntries() const { return vectorEntries_; }
    /// Self-modifying code: instruction start -> instructions that write into it.
    [[nodiscard]] const std::map<Address, std::set<Address>>& selfModified() const { return selfMod_; }

    /// Types of the bytes that are not code: text, screen codes, zero runs, word and pointer tables,
    /// sprites, character sets, a BASIC program. `claimed(a)` is true for bytes that are annotated already.
    [[nodiscard]] std::vector<DataGuess> guessData(const std::function<bool(Address)>& claimed = nullptr) const;

    /// Routines ordered by address; `depth` over the call graph (cycles are cut).
    [[nodiscard]] std::vector<RoutineInfo> routines() const;

    /// True for instructions that write their memory operand (stores, read-modify-write).
    [[nodiscard]] static bool writesMemory(const Instruction& in);
    /// True for instructions that read their memory operand.
    [[nodiscard]] static bool readsMemory(const Instruction& in);

private:
    [[nodiscard]] std::uint8_t state(Address a) const { return state_[a - from_]; }
    [[nodiscard]] std::optional<Address> previous(Address a) const;
    bool place(const Instruction& in);
    void follow(std::vector<Address>& work);
    void detectTables(std::vector<Address>& work);
    void detectImmediatePairs(std::vector<Address>* work);
    void detectSelfModification();
    void scanPointerTables();
    bool addTable(JumpTable t, std::vector<Address>& work);
    unsigned tableLength(Address lo, Address hi, unsigned stride, bool minusOne) const;

    const Memory& mem_;
    Address from_;
    std::uint32_t to_;
    std::function<bool(Address)> isData_;
    std::vector<std::uint8_t> state_;  // 0 unknown, 1 instruction start, 2 operand byte, 3 jump table data
    std::map<Address, Instruction> code_;
    std::vector<JumpTable> tables_;
    std::set<Address> entries_, callTargets_, jumpTargets_, tableTargets_, refs_, handledJumps_;
    std::map<Address, std::set<Address>> callers_;  // call target -> call instructions
    std::map<Address, std::pair<Address, bool>> immediates_;
    std::map<Address, std::set<Address>> selfMod_;
    std::map<Address, Address> vectorEntries_;
    std::set<Address> codeHints_;
    void adoptOrphans();
};

}  // namespace m6510
