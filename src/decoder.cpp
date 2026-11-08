#include "m6510/decoder.hpp"

namespace m6510 {

namespace {

Mode zeroPageOf(Mode m) {
    switch (m) {
        case Mode::Absolute: return Mode::ZeroPage;
        case Mode::AbsoluteX: return Mode::ZeroPageX;
        case Mode::AbsoluteY: return Mode::ZeroPageY;
        default: return m;
    }
}

FlowKind flowOf(const OpInfo& op, Byte opcode) {
    if (op.kind == OpKind::Jam) return FlowKind::Halt;
    if (op.mode == Mode::Relative) return FlowKind::CondJump;
    switch (opcode) {
        case 0x4C:
        case 0x6C: return FlowKind::Jump;
        case 0x20: return FlowKind::Call;
        case 0x60:
        case 0x40:
        case 0x00: return FlowKind::Return;
        default: return FlowKind::Normal;
    }
}

}  // namespace

bool hasZeroPageForm(const std::string& mnemonic, Mode absoluteMode) {
    const Mode zp = zeroPageOf(absoluteMode);
    return zp != absoluteMode && findOpcode(mnemonic, zp).has_value();
}

Instruction decode(const Memory& memory, Address address, const DecodeOptions& options) {
    Instruction in;
    in.address = address;
    const Byte opcode = memory.read8(address);
    in.opcode = opcode;
    const OpInfo& op = opInfo(opcode);

    auto name = [&](Address a, int digits) {
        if (options.symbolizer) {
            std::string s = options.symbolizer(a);
            if (!s.empty()) return s;
        }
        return formatHex(a, digits, options.style);
    };

    if (!allowedAt(opcode, options.level)) {
        in.bytes = {opcode};
        in.mnemonic = ".BYTE";
        in.operands = formatHex(opcode, 2, options.style);
        in.value = opcode;
        return in;
    }

    const unsigned len = modeLength(op.mode);
    in.bytes = memory.readBytes(address, len);
    in.valid = true;
    in.canonical = op.preferred;
    in.mode = op.mode;
    in.mnemonic = op.mnemonic;
    in.flow = flowOf(op, opcode);

    const unsigned lo = len > 1 ? in.bytes[1] : 0;
    const unsigned word = len > 2 ? (lo | (unsigned{in.bytes[2]} << 8)) : lo;
    in.value = word;

    switch (op.mode) {
        case Mode::Implied:
            break;
        case Mode::Accumulator:
            in.operands = "A";
            break;
        case Mode::Immediate:
            in.operands = "#" + formatHex(lo, 2, options.style);
            break;
        case Mode::ZeroPage:
        case Mode::ZeroPageX:
        case Mode::ZeroPageY: {
            in.reference = static_cast<Address>(lo);
            in.operands = name(static_cast<Address>(lo), 2);
            if (op.mode == Mode::ZeroPageX) in.operands += ",X";
            if (op.mode == Mode::ZeroPageY) in.operands += ",Y";
            break;
        }
        case Mode::Absolute:
        case Mode::AbsoluteX:
        case Mode::AbsoluteY: {
            const auto a = static_cast<Address>(word);
            if (in.flow == FlowKind::Jump || in.flow == FlowKind::Call) {
                in.target = a;
            } else {
                in.reference = a;
            }
            in.operands = name(a, 4);
            if (op.mode == Mode::AbsoluteX) in.operands += ",X";
            if (op.mode == Mode::AbsoluteY) in.operands += ",Y";
            // The assembler would pick the zero-page form for a small value: force absolute.
            if (word < 0x100 && hasZeroPageForm(op.mnemonic, op.mode)) in.mnemonic += ".W";
            break;
        }
        case Mode::Indirect:
            in.reference = static_cast<Address>(word);
            in.operands = "(" + name(static_cast<Address>(word), 4) + ")";
            break;
        case Mode::IndirectX:
            in.reference = static_cast<Address>(lo);
            in.operands = "(" + name(static_cast<Address>(lo), 2) + ",X)";
            break;
        case Mode::IndirectY:
            in.reference = static_cast<Address>(lo);
            in.operands = "(" + name(static_cast<Address>(lo), 2) + "),Y";
            break;
        case Mode::Relative: {
            const int offset = lo >= 0x80 ? static_cast<int>(lo) - 0x100 : static_cast<int>(lo);
            const auto t = static_cast<Address>(address + 2 + offset);
            in.target = t;
            in.value = t;
            in.operands = name(t, 4);
            break;
        }
    }
    return in;
}

}  // namespace m6510
