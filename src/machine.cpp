#include "m6510/machine.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "m6510/petscii.hpp"

namespace m6510 {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Machine::Machine() { select(false, false); }

Machine::~Machine() {
    kernal_.reset();
    c64_.reset();
}

void Machine::select(bool c64, bool kernalEmulation) {
    kernal_.reset();
    c64_.reset();
    cpu_.reset();
    memory_ = std::make_unique<Memory>();
    cpu_ = std::make_unique<Cpu>(*memory_);
    symbols.clear();
    programRange.reset();
    checkpoints.clear();
    watches_.clear();
    attached_.clear();
    if (!c64) {
        cpu_->setPortEnabled(false);  // generic machine: 64 KiB RAM everywhere, $00/$01 are RAM
        cpu_->powerOn();
        cpu_->registers().s = 0xFF;
        return;
    }
    c64_ = std::make_unique<C64>(*memory_, *cpu_);
    if (kernalEmulation) {
        kernal_ = std::make_unique<Kernal>(*c64_, *cpu_, *memory_);
        kernal_->install();
    }
    reset(false);
}

void Machine::reset(bool clearMemory) {
    if (clearMemory) memory_->clear();
    CpuState c = cpu_->state();
    c.instructions = c.cycles = c.interrupts = 0;
    c.depth = 0;
    c.halted = false;
    c.irqLine = c.nmiPending = false;
    cpu_->setState(c);
    if (!c64_) {
        cpu_->powerOn();
        cpu_->registers().s = 0xFF;
        return;
    }
    cpu_->powerOn();
    c64_->resetChips();
    if (kernal_) {
        kernal_->coldStart();
        (void)c64_->takeIoLog();
        (void)kernal_->takeCalls();
        cpu_->registers().pc = Kernal::kExitTrap;
    }
}

Byte Machine::peek(Address a) const { return c64_ ? c64_->peek(a) : cpu_->peek(a); }

void Machine::poke(Address a, Byte v) {
    if (c64_) c64_->write(a, v);
    else memory_->write8(a, v);
}

Memory Machine::view(Address start, unsigned count) const {
    Memory v;
    for (unsigned i = 0; i < count; ++i) {
        const auto a = static_cast<Address>(start + i);
        v.write8(a, peek(a));
    }
    return v;
}

Snapshot Machine::capture() const {
    Snapshot snap;
    snap.cpu = cpu_->state();
    snap.memory = *memory_;
    if (c64_) snap.c64 = c64_->state();
    if (kernal_) snap.kernal = kernal_->state();
    return snap;
}

void Machine::restore(const Snapshot& snap) {
    if (snap.c64.has_value() != (c64_ != nullptr)) {
        throw std::runtime_error(std::string("the snapshot is for the ") + (snap.c64 ? "c64" : "generic") +
                                 " machine; select it first");
    }
    *memory_ = snap.memory;
    cpu_->setState(snap.cpu);
    if (c64_ && snap.c64) c64_->setState(*snap.c64);
    if (kernal_ && snap.kernal) kernal_->setState(*snap.kernal);
}

void Machine::loadRom(const std::string& which, const std::vector<Byte>& image) {
    if (!c64_) throw std::runtime_error("ROM images need the C64 machine");
    c64_->loadRom(which, image);
    if (which == "kernal") {
        kernal_.reset();  // the real KERNAL replaces the emulation
        cpu_->executeHook = nullptr;
        cpu_->hookLow = 1;
        cpu_->hookHigh = 0;
        cpu_->reset();
    }
}

void Machine::attach(const std::string& path) {
    if (!c64_ || !kernal_) throw std::runtime_error("attaching files needs the C64 machine with the KERNAL emulation");
    namespace fs = std::filesystem;
    if (!fs::exists(path)) throw std::runtime_error("'" + path + "' does not exist");
    attached_ = path;
    kernal_->files = [path](const std::string& name, unsigned device) -> std::optional<Program> {
        if (device < 8) return std::nullopt;
        try {
            if (fs::is_directory(path)) {
                std::vector<fs::path> prgs;
                for (const auto& e : fs::directory_iterator(path)) {
                    const std::string ext = fileExtension(e.path().string());
                    if (e.is_regular_file() && (ext == ".prg" || ext == ".p00")) prgs.push_back(e.path());
                }
                std::sort(prgs.begin(), prgs.end());
                for (const fs::path& f : prgs) {
                    const std::string stem = upper(f.stem().string());
                    if (name == "*" || name.empty() || stem == upper(name)) return loadProgramFile(f.string());
                }
                return std::nullopt;
            }
            return loadProgramFile(path);  // a single file answers every name
        } catch (const IoError&) {
            return std::nullopt;
        }
    };
}

LoadedFile Machine::loadFile(const std::string& path, const std::string& requestedType, std::optional<Address> address) {
    std::string type = requestedType;
    const std::vector<Byte> data = readBinaryFile(path);
    if (type == "auto") {
        const std::string ext = fileExtension(path);
        if (looksLikeP00(data)) type = "p00";
        else if (ext == ".bin" || ext == ".raw") type = "raw";
        else if (ext == ".d64" || ext == ".t64" || ext == ".crt") type = ext.substr(1);
        else type = "prg";
    }
    LoadedFile out;
    Program& p = out.program;
    if (type == "raw") {
        if (!address) throw std::runtime_error("a raw file needs a load address");
        p.address = *address;
        p.bytes = data;
    } else if (type == "prg" || type == "p00") {
        p = type == "prg" ? parsePrg(data) : parseP00(data);
        if (address) p.address = *address;
    } else if (type == "d64" || type == "t64" || type == "crt") {
        throw std::runtime_error(type + " images are planned for milestone 0.6 (docs/roadmap.md)");
    } else {
        throw std::runtime_error("unknown type '" + type + "' (auto, raw, prg, p00)");
    }
    if (p.end() > 0x10000) throw std::runtime_error("the program does not fit below $FFFF");
    loadProgram(*memory_, p);
    if (!p.bytes.empty()) programRange = std::make_pair(p.address, static_cast<std::uint32_t>(p.end()));
    out.entry = p.address;
    if (p.address == 0x0801) {
        if (auto t = findSysTarget(*memory_, p.address)) {
            out.entry = *t;
            std::string error;
            const auto lines = detokenizeBasic(*memory_, p.address, &error, 1);
            if (!lines.empty()) out.basic = std::to_string(lines[0].number) + " " + lines[0].text;
        }
    }
    cpu_->registers().pc = out.entry;
    cpu_->setHalted(false);
    return out;
}

bool Machine::lookupSymbol(const std::string& name, std::int64_t& value) const {
    if (auto it = symbols.find(name); it != symbols.end()) {
        value = it->second;
        return true;
    }
    if (c64_) {
        const std::string u = upper(name);
        for (const auto& [a, n] : Kernal::jumpTable()) {
            if (n == u) {
                value = a;
                return true;
            }
        }
    }
    return false;
}

std::map<Address, std::string> Machine::reverseSymbols() const {
    std::map<Address, std::string> names;
    if (c64_)
        for (const auto& [a, n] : Kernal::jumpTable()) names[a] = n;
    std::map<Address, bool> user;  // first (alphabetical) user symbol per address wins; local labels only as fallback
    for (const auto& [n, v] : symbols) {
        if (v < 0 || v > 0xFFFF) continue;
        const auto a = static_cast<Address>(v);
        const bool local = n.find('.') != std::string::npos;
        auto it = names.find(a);
        if (it == names.end() || !user[a] || (it->second.find('.') != std::string::npos && !local)) {
            names[a] = n;
            user[a] = true;
        }
    }
    return names;
}

std::map<std::string, std::int64_t> Machine::assemblerSymbols() const {
    std::map<std::string, std::int64_t> s = symbols;
    if (c64_)
        for (const auto& [addr, n] : Kernal::jumpTable()) s.emplace(n, addr);  // JSR CHROUT
    return s;
}

void Machine::addWatch(const WatchSpec& w) {
    watches_.push_back(w);
    applyWatches();
}

void Machine::removeWatch(Address start) {
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [start](const WatchSpec& w) { return w.start == start; }),
                   watches_.end());
    applyWatches();
}

void Machine::clearWatches() {
    watches_.clear();
    applyWatches();
}

void Machine::applyWatches() {
    cpu_->clearWatchpoints();
    for (const WatchSpec& w : watches_) cpu_->addWatchpoint(w.start, w.length, w.read, w.write);
}

std::vector<std::string> Machine::screenLines() const {
    std::vector<std::string> lines;
    if (!c64_) return lines;
    const Address screen = c64_->screenAddress();
    for (unsigned row = 0; row < 25; ++row) {
        std::string l;
        for (unsigned col = 0; col < 40; ++col)
            l += screenCodeToChar(memory_->read8(static_cast<Address>(screen + row * 40 + col)));
        while (!l.empty() && l.back() == ' ') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

std::string Machine::screenMode() const {
    if (!c64_) return "";
    const Byte d011 = c64_->vic(0x11), d016 = c64_->vic(0x16);
    if (d011 & 0x20) return (d016 & 0x10) ? "multicolor bitmap" : "hires bitmap";
    if (d011 & 0x40) return "extended background text";
    return (d016 & 0x10) ? "multicolor text" : "text";
}

}  // namespace m6510
