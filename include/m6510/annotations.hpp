#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "m6510/json.hpp"
#include "m6510/types.hpp"

namespace m6510 {

/// Documentation of a subroutine (shown as a header block above its label).
struct RoutineDoc {
    std::string name;
    std::string summary;     ///< what it does
    std::string inputs;      ///< e.g. "A = character, X = column"
    std::string outputs;     ///< e.g. "C = 1 on error"
    std::string clobbers;    ///< e.g. "A Y"
    std::string confidence;  ///< "", "high" (verified by a test), "medium" or "low" (a guess, to be checked)
};

/// A named memory location: a variable (zero page or absolute), a pointer (size 2) or a buffer.
struct VariableDoc {
    std::string name;
    Address address = 0;
    unsigned size = 1;  ///< bytes
    std::string comment;
};

enum class DataKind {
    Bytes,
    Words,       ///< 16-bit numbers (little-endian)
    Text,        ///< PETSCII
    Screen,      ///< screen codes
    Pointers,    ///< 16-bit addresses of code (followed by the analysis)
    RtsPointers, ///< 16-bit addresses of code minus one (for the RTS trick; followed)
    Lohi,        ///< split table: low bytes here, high bytes at `partner` (`length` entries); targets followed
    Sprites,     ///< 64-byte sprite blocks (63 bytes of pixels + 1)
    Charset      ///< character set: 8 bytes per character
};
const char* toString(DataKind kind);
std::optional<DataKind> dataKindFromString(const std::string& text);

/// A range that is data, not code, and how to show it.
struct DataRegion {
    Address address = 0;
    unsigned length = 0;   ///< bytes; for Lohi the number of entries (= bytes of each of the two tables)
    DataKind kind = DataKind::Bytes;
    Address partner = 0;   ///< Lohi: the table of high bytes
    bool minusOne = false; ///< Lohi: the entries are address - 1 (RTS trick)
    std::string name;      ///< optional label at the start
    std::string comment;

    /// Bytes covered at `address` (and, for Lohi, as many at `partner`).
    [[nodiscard]] bool covers(Address a) const {
        if (a >= address && a < address + length) return true;
        return kind == DataKind::Lohi && a >= partner && a < partner + length;
    }
};

/// Everything a human or an LLM has learned about a program: names and comments. It contains no code;
/// the listing is always generated from the real bytes, so annotations cannot change what the program
/// does (the listing is assembled again and compared, see renderListing). Design and file format follow
/// the MC68000 project.
class Annotations {
public:
    /// All setters return an error message, or nullopt on success.
    std::optional<std::string> setLabel(Address address, const std::string& name);
    std::optional<std::string> setRoutine(Address address, const RoutineDoc& doc);
    std::optional<std::string> setVariable(const VariableDoc& variable);
    std::optional<std::string> setData(const DataRegion& region);
    void setComment(Address address, const std::string& text) { comments_[address] = text; }
    void setBlockComment(Address address, const std::string& text) { blockComments_[address] = text; }
    /// Marks the instruction at `address` as one whose immediate operand is a plain number: the listing
    /// then does not write it as `#<label` / `#>label`.
    void setConstant(Address address, bool on) {
        if (on) constants_.insert(address);
        else constants_.erase(address);
    }
    void setTitle(const std::string& title) { title_ = title; }

    bool removeLabel(Address address);  ///< also removes a routine/variable/data region at that address
    bool removeComment(Address address);
    void clear();

    /// Checks a name without registering it: a valid assembler symbol that is not a register, mnemonic,
    /// KERNAL entry or chip register name, or a generated placeholder (`sub_C000`).
    [[nodiscard]] static std::optional<std::string> checkName(const std::string& name);

    // ---- lookups ----
    /// The name given to exactly this address (label, routine, variable or data region), or nullopt.
    [[nodiscard]] std::optional<std::string> nameAt(Address address) const;
    /// "name" or "name+k" if `address` lies inside a named variable/data region.
    [[nodiscard]] std::optional<std::string> expressionFor(Address address) const;
    [[nodiscard]] std::optional<Address> addressOf(const std::string& name) const;
    [[nodiscard]] const RoutineDoc* routineAt(Address address) const;
    [[nodiscard]] const std::string* commentAt(Address address) const;
    [[nodiscard]] const std::string* blockCommentAt(Address address) const;
    [[nodiscard]] bool isConstant(Address address) const { return constants_.count(address) != 0; }
    [[nodiscard]] const DataRegion* dataAt(Address address) const;        ///< region *starting* at address (or its Lohi partner)
    [[nodiscard]] const DataRegion* dataCovering(Address address) const;  ///< region that contains address
    [[nodiscard]] const VariableDoc* variableAt(Address address) const;   ///< variable *starting* at address

    [[nodiscard]] const std::map<Address, std::string>& labels() const { return labels_; }
    [[nodiscard]] const std::map<Address, RoutineDoc>& routines() const { return routines_; }
    [[nodiscard]] const std::vector<VariableDoc>& variables() const { return variables_; }
    [[nodiscard]] const std::map<Address, DataRegion>& dataRegions() const { return data_; }
    [[nodiscard]] const std::map<Address, std::string>& comments() const { return comments_; }
    [[nodiscard]] const std::map<Address, std::string>& blockComments() const { return blockComments_; }
    [[nodiscard]] const std::set<Address>& constants() const { return constants_; }
    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] Json toJson() const;
    /// Throws JsonError on malformed input or rule violations (duplicate names, ...).
    static Annotations fromJson(const Json& json);

private:
    std::optional<std::string> claimName(Address address, const std::string& name);

    std::map<Address, std::string> labels_;  ///< address -> name (also routine/variable/data names)
    std::map<std::string, Address> byName_;  ///< upper-case name -> address
    std::map<Address, RoutineDoc> routines_;
    std::vector<VariableDoc> variables_;
    std::map<Address, DataRegion> data_;
    std::map<Address, std::string> comments_;
    std::map<Address, std::string> blockComments_;
    std::set<Address> constants_;
    std::string title_;
};

}  // namespace m6510
