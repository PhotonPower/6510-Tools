#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "m6510/cpu.hpp"
#include "m6510/memory.hpp"

namespace m6510 {

/// One write to a chip register (VIC-II, SID, CIA 1/2, I/O 1/2) or colour RAM.
struct IoWrite {
    Address address = 0;  ///< the register (mirrors mapped to the base register)
    Byte value = 0;
    Address pc = 0;       ///< address of the writing instruction
    std::string chip;     ///< "VIC", "SID", "CIA1", "CIA2", "IO1", "IO2", "COLOR"
    std::string name;     ///< register name ("EXTCOL"), empty for colour RAM and I/O 1/2
};

/// Commodore 64 memory map (docs/c64.md): RAM, ROMs, I/O selected by the processor port ($01),
/// colour RAM, chip registers as memory with a write log, a raster counter and CIA 1 timer A derived
/// from the cycle count (raster and timer interrupts), no other device models.
///
/// The machine attaches itself to a `Cpu` (read/write overrides, port hook, trace hook). Writes always
/// reach the RAM underneath ROM; with I/O visible they go to the chip registers instead.
class C64 {
public:
    static constexpr unsigned kRasterLines = 312;  ///< PAL
    static constexpr unsigned kCyclesPerLine = 63;

    C64(Memory& ram, Cpu& cpu);
    C64(const C64&) = delete;
    C64& operator=(const C64&) = delete;
    ~C64();

    // ---- ROMs ------------------------------------------------------------------------------------
    /// Loads a ROM image: "basic" (8 KiB at $A000), "kernal" (8 KiB at $E000), "char" (4 KiB).
    /// Throws std::invalid_argument for a wrong name or size.
    void loadRom(const std::string& which, const std::vector<Byte>& image);
    [[nodiscard]] bool hasBasic() const { return hasBasic_; }
    [[nodiscard]] bool hasKernal() const { return hasKernal_; }
    [[nodiscard]] bool hasChar() const { return hasChar_; }
    [[nodiscard]] const std::array<Byte, 0x1000>& charRom() const { return char_; }
    /// The KERNAL image in use (a real one, or the one generated for the KERNAL emulation).
    [[nodiscard]] const std::array<Byte, 0x2000>& kernalRom() const { return kernal_; }
    std::array<Byte, 0x2000>& kernalRomForEmulation() { return kernal_; }
    void setKernalPresent(bool on) { hasKernal_ = on; }

    // ---- input: joysticks and the keyboard matrix as the CIA 1 ports see them ------------------------
    /// Joystick bits (1 = pressed): 1 up, 2 down, 4 left, 8 right, 16 fire. Port 2 is read at $DC00, port 1 at $DC01.
    void setJoystick(unsigned port, Byte pressed);
    [[nodiscard]] Byte joystick(unsigned port) const { return port == 1 ? joy1_ : joy2_; }
    /// Presses (or releases) a key of the 8x8 matrix: column = PA bit ($DC00), row = PB bit ($DC01).
    void setKey(unsigned column, unsigned row, bool pressed);
    void releaseAllKeys() { keys_.fill(0); }
    /// Matrix position of a key name ("A", "1", "SPACE", "RETURN", "F1", "RUNSTOP", "LSHIFT", "CRSRDOWN", ...); false if unknown.
    static bool keyPosition(const std::string& name, unsigned& column, unsigned& row);
    /// The keys pressed now, by name.
    [[nodiscard]] std::vector<std::string> pressedKeys() const;

    // ---- cartridge (CRT hardware type 0: 8K, 16K, Ultimax) ------------------------------------------
    /// Inserts a cartridge: ROML (at $8000) and ROMH (at $A000, or $E000 in Ultimax mode), each at most 8 KiB, and the
    /// EXROM/GAME lines (true = active, i.e. low). 8K: EXROM; 16K: EXROM and GAME; Ultimax: GAME only.
    void insertCartridge(std::vector<Byte> roml, std::vector<Byte> romh, bool exrom, bool game);
    void removeCartridge();
    [[nodiscard]] bool ultimax() const { return cartGame_ && !cartExrom_; }
    /// "none", "8K", "16K" or "Ultimax".
    [[nodiscard]] std::string cartridgeMode() const;

    // ---- memory configuration ------------------------------------------------------------------
    /// LORAM/HIRAM/CHAREN as the processor port drives them (bits 0-2).
    [[nodiscard]] Byte config() const { return config_; }
    [[nodiscard]] bool basicVisible() const { return hasBasic_ && (config_ & 3) == 3 && !cartGame_; }
    [[nodiscard]] bool kernalVisible() const { return hasKernal_ && (config_ & 2) != 0 && !ultimax(); }
    [[nodiscard]] bool ioVisible() const { return (config_ & 3) != 0 && (config_ & 4) != 0; }
    [[nodiscard]] bool charVisible() const { return hasChar_ && (config_ & 3) != 0 && (config_ & 4) == 0; }
    /// What the processor sees at `address` (no side effects).
    [[nodiscard]] Byte peek(Address address) const;
    /// Text description of the current configuration ("$A000:BASIC $D000:I/O $E000:KERNAL").
    [[nodiscard]] std::string describeConfig() const;
    /// Writes like the processor would (port at $00/$01, I/O registers with log entry, RAM under ROM).
    void write(Address address, Byte value);

    // ---- chips -------------------------------------------------------------------------------------
    [[nodiscard]] Byte colorRam(unsigned index) const { return color_[index & 0x3FF]; }
    void setColorRam(unsigned index, Byte v) { color_[index & 0x3FF] = static_cast<Byte>(v & 0x0F); }
    [[nodiscard]] Byte vic(unsigned reg) const { return vic_[reg & 0x3F]; }
    [[nodiscard]] unsigned rasterLine() const;
    /// Start address of the VIC-II's 16 KiB bank ($DD00 bits 0-1, inverted).
    [[nodiscard]] Address vicBank() const;
    /// Address of the text screen as the VIC sees it ($D018 and the bank).
    [[nodiscard]] Address screenAddress() const;

    /// Write log since the last `takeIoLog()` (at most `maxIoLog` newest entries).
    std::vector<IoWrite> takeIoLog();
    std::size_t maxIoLog = 4096;
    /// Name of a chip register ("EXTCOL"), or "" if none.
    [[nodiscard]] static std::string registerName(Address address);

    /// Device state for checkpoints and snapshots.
    struct State {
        std::array<Byte, 0x400> color{};
        std::array<Byte, 0x40> vic{};
        std::array<Byte, 0x20> sid{};
        std::array<Byte, 0x10> cia1{}, cia2{};
        Byte config = 7;
        Byte vicIrq = 0;          ///< $D019 latch
        Byte cia1Icr = 0, cia1Mask = 0;
        /// The four CIA timers: CIA 1 A, CIA 1 B, CIA 2 A, CIA 2 B.
        struct Timer {
            std::uint16_t latch = 0xFFFF;
            std::uint16_t counter = 0xFFFF;  ///< value at `start` (running) or now (stopped)
            std::uint64_t start = 0;         ///< cycle at which the counter had the value `counter`
            std::uint64_t next = 0;          ///< cycle of the next underflow (counting cycles)
        };
        std::array<Timer, 4> timers{};
        std::uint64_t lastRasterCheck = 0;
        Byte cia2Icr = 0, cia2Mask = 0;
        bool nmiLine = false;     ///< CIA 2 interrupt output (the NMI input is edge-triggered)
        std::uint32_t random = 0x12345678;
        bool externalIrq = false;  ///< IRQ requested from outside (session, monitor)
    };
    [[nodiscard]] State state() const;
    void setState(const State& s);
    /// Holds the IRQ line active in addition to the chips (until released).
    void setExternalIrq(bool on) {
        s_.externalIrq = on;
        updateIrq();
    }
    /// Power-on values of the chips (all registers 0, colour RAM 0, config from the port).
    void resetChips();

private:
    friend class C64Access;
    Memory& ram_;
    Cpu& cpu_;
    std::array<Byte, 0x2000> basic_{}, kernal_{};
    std::array<Byte, 0x1000> char_{};
    bool hasBasic_ = false, hasKernal_ = false, hasChar_ = false;
    State s_;
    std::vector<Byte> roml_, romh_;
    Byte joy1_ = 0, joy2_ = 0;            // external inputs: not part of the state (like a person at the joystick)
    std::array<Byte, 8> keys_{};          // pressed keys: column -> row bits
    bool cartExrom_ = false, cartGame_ = false;  // lines active
    bool readMapped(Address a, Byte& v, bool effects);
    Byte& config_ = s_.config;
    std::array<Byte, 0x400>& color_ = s_.color;
    std::array<Byte, 0x40>& vic_ = s_.vic;
    std::vector<IoWrite> log_;

    bool readIo(Address a, Byte& v, bool effects = true);
    bool writeIo(Address a, Byte v);
    void tick();
    void updateIrq();
    /// Current value of timer `index` (0 = CIA 1 A, 1 = CIA 1 B, 2 = CIA 2 A, 3 = CIA 2 B).
    [[nodiscard]] unsigned timerValue(unsigned index) const;
    [[nodiscard]] Byte& control(unsigned index) { return (index < 2 ? s_.cia1 : s_.cia2)[0x0E + (index & 1)]; }
    [[nodiscard]] Byte control(unsigned index) const { return (index < 2 ? s_.cia1 : s_.cia2)[0x0E + (index & 1)]; }
    [[nodiscard]] bool countsCycles(unsigned index) const { return !(index & 1) || (control(index) & 0x60) == 0; }
    void timerUnderflows(unsigned index, std::uint64_t n);
    void writeTimer(unsigned cia, unsigned r, Byte v);
    void updateNmi();
};

}  // namespace m6510
