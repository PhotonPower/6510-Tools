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

    SessionOptions options_;
    Machine m_;
    std::map<std::string, IndexedImage> screens_;  // named screenshots (full screen) for screen_diff
};

}  // namespace m6510
