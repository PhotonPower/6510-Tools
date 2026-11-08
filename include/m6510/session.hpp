#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "m6510/c64.hpp"
#include "m6510/cpu.hpp"
#include "m6510/json.hpp"
#include "m6510/kernal.hpp"
#include "m6510/memory.hpp"
#include "m6510/snapshot.hpp"

namespace m6510 {

struct SessionOptions {
    /// Allow commands that read/write files on the host (load_file, load_rom, attach, save_state ...).
    /// Off by default: an agent gets no file access unless the operator opts in.
    bool allowFiles = false;
};

/// One emulated machine plus what an agent needs to work with it: symbols, named checkpoints and a
/// JSON command interface (docs/agent-protocol.md).
///
/// A request is a JSON object {"cmd": "...", ...}; the reply is a JSON object with "ok": true plus the
/// results, or "ok": false and "error". `handle` never throws.
class Session {
public:
    explicit Session(SessionOptions options = {});
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    Json handle(const Json& request);
    /// Parses one line of JSON, handles it and returns the reply as a single line.
    std::string handleLine(std::string_view line);

    [[nodiscard]] Cpu& cpu() { return *cpu_; }
    [[nodiscard]] Memory& memory() { return *memory_; }
    [[nodiscard]] C64* c64() { return c64_.get(); }
    [[nodiscard]] Kernal* kernal() { return kernal_.get(); }

private:
    struct Impl;
    friend struct Impl;

    struct Watch {
        Address start;
        unsigned length;
        bool read, write;
    };

    void selectMachine(bool c64, bool hle);
    [[nodiscard]] Snapshot capture() const;
    void restore(const Snapshot& s);
    void applyWatches();

    SessionOptions options_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<C64> c64_;        // null = generic machine
    std::unique_ptr<Kernal> kernal_;  // null = no KERNAL emulation (generic machine or a real KERNAL ROM)
    std::string attached_;            // file or directory that LOAD reads from
    std::map<std::string, std::int64_t> symbols_;
    std::map<std::string, Snapshot> checkpoints_;
    std::vector<Watch> watches_;
};

}  // namespace m6510
