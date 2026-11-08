#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "m6510/analysis.hpp"
#include "m6510/annotations.hpp"
#include "m6510/c64.hpp"
#include "m6510/cpu.hpp"
#include "m6510/json.hpp"
#include "m6510/kernal.hpp"
#include "m6510/machine.hpp"
#include "m6510/memory.hpp"
#include "m6510/snapshot.hpp"
#include "m6510/video.hpp"

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

    [[nodiscard]] Cpu& cpu() { return m_.cpu(); }
    [[nodiscard]] Memory& memory() { return m_.memory(); }
    [[nodiscard]] C64* c64() { return m_.c64(); }
    [[nodiscard]] Kernal* kernal() { return m_.kernal(); }
    [[nodiscard]] Machine& machine() { return m_; }

private:
    struct Impl;
    friend struct Impl;

    /// The reverse-engineering commands (src/session_analysis.cpp).
    Json analysisCommand(const std::string& cmd, const Json& request);
    [[nodiscard]] static bool isAnalysisCommand(const std::string& cmd);
    /// A number or expression argument (symbols, registers) as in every other command.
    std::int64_t argValue(const Json& value, const std::string& what);
    void resetAnalysis();

    SessionOptions options_;
    Machine m_;
    Annotations ann_;
    std::unique_ptr<Memory> analysisView_;  // the RAM as it was at the last analyze
    std::unique_ptr<Analysis> analysis_;
    std::set<Address> analysisEntries_;
    std::vector<std::pair<Address, std::uint32_t>> linearRanges_;
    std::map<std::string, IndexedImage> screens_;  // named screenshots (full screen) for screen_diff
};

}  // namespace m6510
