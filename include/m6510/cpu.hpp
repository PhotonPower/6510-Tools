#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "m6510/memory.hpp"
#include "m6510/types.hpp"

namespace m6510 {

struct Registers {
    Byte a = 0, x = 0, y = 0;
    Byte s = 0xFD;
    Byte p = kFlagU | kFlagI;  ///< bit 5 always reads as 1, B only exists on the stack
    Address pc = 0;

    [[nodiscard]] bool flag(Byte f) const { return (p & f) != 0; }
};

enum class StopReason {
    None,        ///< one instruction was executed
    Limit,       ///< the instruction limit was reached
    Breakpoint,  ///< pc is at a breakpoint (the instruction there has not run)
    Watchpoint,  ///< the last instruction touched a watched address
    Reached,     ///< pc reached the requested address
    Returned,    ///< the call depth dropped below its starting value (RTS/RTI)
    Halted,      ///< a JAM opcode stopped the processor
    Requested    ///< an execute hook asked to stop (see Cpu::stopMessage())
};

struct StepResult {
    StopReason reason = StopReason::None;
    unsigned cycles = 0;        ///< cycles of the instruction (or interrupt sequence)
    bool interrupt = false;     ///< an IRQ or NMI was taken instead of an instruction
};

struct RunResult {
    StopReason reason = StopReason::None;
    std::uint64_t instructions = 0;  ///< instructions executed by this call
    std::uint64_t cycles = 0;        ///< cycles used by this call
    Address watchAddress = 0;        ///< for Watchpoint: the address that was touched
};

/// Everything that defines the processor's state besides the memory (breakpoints and hooks are not part of it).
struct CpuState {
    Registers regs;
    bool halted = false;
    std::uint64_t instructions = 0;
    std::uint64_t cycles = 0;
    std::uint64_t interrupts = 0;  ///< IRQs, NMIs and BRKs taken
    int depth = 0;                 ///< call depth (see Cpu::runUntilReturn)
    bool irqLine = false;          ///< IRQ input (level)
    bool nmiPending = false;       ///< NMI edge seen, not yet taken
    bool irqInhibit = false;       ///< I flag value the next interrupt poll uses (CLI/SEI/PLP delay)
    bool portEnabled = true;
    Byte portDirection = 0;        ///< $00
    Byte portData = 0;             ///< $01 output latch
};

/// NMOS 6510 processor core (see docs/emulator.md).
///
/// Instruction-exact: registers, flags, memory and the number of cycles of every instruction are
/// right, including the NMOS decimal mode, all undocumented opcodes (unstable ones as described in
/// docs/emulator.md) and the page-wrap quirks. Individual bus cycles are not modelled, apart from the
/// dummy write of read-modify-write instructions, which devices see (`INC $D019`).
///
/// The 6510's I/O port ($00 direction, $01 data) is part of the core and can be disabled for a plain
/// 6502. Writes to $00/$01 also reach the RAM underneath.
class Cpu {
public:
    explicit Cpu(Memory& memory);

    Registers& registers() { return r_; }
    [[nodiscard]] const Registers& registers() const { return r_; }
    Memory& memory() { return mem_; }
    [[nodiscard]] const Memory& memory() const { return mem_; }
    /// Address of the instruction being (or last) executed; valid inside hooks.
    [[nodiscard]] Address instructionAddress() const { return startPc_; }

    /// RESET: I = 1, S decremented by 3, PC from $FFFC. The other registers keep their values;
    /// `powerOn()` sets the documented power-on values first (A = X = Y = 0, P = $24, S = $FD before the reset).
    void reset();
    void powerOn();

    // ---- running ----------------------------------------------------------------------------------
    /// Executes one instruction (or takes a pending interrupt). Breakpoints are ignored.
    StepResult step();
    /// Runs at most `maxInstructions`; stops at breakpoints, watchpoints and JAM. The instruction at the
    /// starting pc runs even if a breakpoint is there, so a stopped program can be continued.
    RunResult run(std::uint64_t maxInstructions);
    /// Runs until pc == `address` (checked before each instruction).
    RunResult runUntil(Address address, std::uint64_t maxInstructions);
    /// Runs until the current subroutine returns (RTS/RTI with the call depth below its start).
    RunResult runUntilReturn(std::uint64_t maxInstructions);
    /// Like step(), but a JSR is run to completion.
    RunResult stepOver(std::uint64_t maxInstructions);
    /// Calls a subroutine like JSR: pushes `returnAddress - 1`, sets pc and runs until pc == `returnAddress`
    /// with S back at its value before the call (only then the result is Returned).
    RunResult call(Address entry, Address returnAddress, std::uint64_t maxInstructions);
    /// Calls an interrupt handler: pushes `returnAddress` and P (with B clear) like the processor does for an IRQ, and with
    /// `kernalFrame` also A, X and Y like the KERNAL's IRQ entry at $FF48 does before it jumps through $0314. Runs until
    /// pc == `returnAddress` with S back at its value before the call (then Returned). Registers and the I flag are not changed.
    RunResult callInterrupt(Address entry, Address returnAddress, bool kernalFrame, std::uint64_t maxInstructions);

    // ---- debugging --------------------------------------------------------------------------------
    void addBreakpoint(Address a) { breakpoints_.insert(a); }
    void removeBreakpoint(Address a) { breakpoints_.erase(a); }
    void clearBreakpoints() { breakpoints_.clear(); }
    [[nodiscard]] const std::set<Address>& breakpoints() const { return breakpoints_; }
    /// Watches [start, start+length) for reads and/or writes; run() stops after the instruction.
    void addWatchpoint(Address start, unsigned length, bool onRead, bool onWrite);
    void clearWatchpoints() { watches_.clear(); }
    struct Watch {
        Address start;
        unsigned length;
        bool onRead, onWrite;
    };
    [[nodiscard]] const std::vector<Watch>& watchpoints() const { return watches_; }

    /// Called before every instruction with its address and opcode.
    std::function<void(const Registers&, Address, Byte)> traceHook;
    /// Called for every data access (not instruction fetches): address, value, true for writes.
    std::function<void(Address, Byte, bool)> accessHook;
    /// Replaces a read (return true and set `value`): ROM, I/O. Used for data reads, instruction fetches
    /// and vector reads. Not consulted for $00/$01 while the I/O port is enabled.
    std::function<bool(Address, Byte& value)> readOverride;
    /// Intercepts a write (return true to swallow it, e.g. an I/O register).
    std::function<bool(Address, Byte)> writeOverride;
    /// Called when the outputs of the I/O port may have changed: (direction, data).
    std::function<void(Byte, Byte)> portHook;
    /// Called instead of executing an instruction when the PC is in [hookLow, hookHigh]. Return true if
    /// it handled the instruction (it must then have moved the PC itself, e.g. with returnFromSubroutine()).
    std::function<bool(Cpu&)> executeHook;
    Address hookLow = 1, hookHigh = 0;  ///< empty range by default

    // ---- interrupts and state ----------------------------------------------------------------------
    void setIrq(bool active) { irqLine_ = active; }
    void nmi() { nmiPending_ = true; }
    [[nodiscard]] bool irqLine() const { return irqLine_; }

    void setPortEnabled(bool on) { portEnabled_ = on; }
    [[nodiscard]] bool portEnabled() const { return portEnabled_; }
    /// What the I/O port inputs read when a pin is not an output (default $17: bits 0-2 and 4 pulled up).
    void setPortInputs(Byte v) { portInputs_ = v; }
    /// The value of $01 as the processor reads it.
    [[nodiscard]] Byte portValue() const;
    [[nodiscard]] Byte portDirection() const { return portDir_; }

    [[nodiscard]] CpuState state() const;
    void setState(const CpuState& s);

    /// For execute hooks: performs an RTS (pops the return address, adjusts the call depth).
    void returnFromSubroutine();
    /// For execute hooks: ends the current run with StopReason::Requested.
    void requestStop(std::string message) {
        stopRequested_ = true;
        stopMessage_ = std::move(message);
    }
    [[nodiscard]] const std::string& stopMessage() const { return stopMessage_; }
    /// Extra cycles a hook wants to account for (added to the current step).
    void addCycles(unsigned n) { extraCycles_ += n; }

    [[nodiscard]] bool halted() const { return halted_; }
    void setHalted(bool h) { halted_ = h; }
    [[nodiscard]] std::uint64_t instructionCount() const { return instructions_; }
    [[nodiscard]] std::uint64_t cycleCount() const { return cycles_; }
    [[nodiscard]] std::uint64_t interruptCount() const { return interrupts_; }
    [[nodiscard]] int callDepth() const { return depth_; }

    /// Reads like the processor (port, overrides) without hooks, watchpoints or side effects.
    [[nodiscard]] Byte peek(Address a) const;

    /// Stack helpers for hooks and the session (push/pull on page 1).
    void push(Byte v);
    Byte pull();

private:
    friend class Exec;

    Memory& mem_;
    Registers r_;
    std::set<Address> breakpoints_;
    std::vector<Watch> watches_;
    bool watchHit_ = false;
    Address watchAddress_ = 0;
    bool halted_ = false;
    bool stopRequested_ = false;
    std::string stopMessage_;
    std::uint64_t instructions_ = 0, cycles_ = 0, interrupts_ = 0;
    unsigned extraCycles_ = 0;
    int depth_ = 0;
    bool irqLine_ = false, nmiPending_ = false, irqInhibit_ = true;
    bool portEnabled_ = true;
    Byte portDir_ = 0, portData_ = 0, portInputs_ = 0x17;
    Address startPc_ = 0;

    Byte read(Address a);         // data read: port, override, memory; hooks and watchpoints
    void write(Address a, Byte v);
    Byte fetch(Address a) const;  // instruction fetch / vector read: port, override, memory
    void interruptSequence(Address vector, bool brk);
    RunResult runLoop(std::uint64_t maxInstructions, std::optional<Address> until, bool untilReturn,
                      std::optional<Byte> untilStack = std::nullopt);
};

}  // namespace m6510
