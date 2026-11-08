#include "m6510/encoder.hpp"

#include <cctype>

#include "m6510/format.hpp"

namespace m6510 {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Index of the bracket that closes the one at `open`, or npos.
std::size_t matching(const std::string& s, std::size_t open) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || (c == '\'' && i + 2 < s.size() && s[i + 2] == '\'')) {
            if (c == '\'') {
                i += 2;
                continue;
            }
            quote = c;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

enum class Shape { None, Acc, Imm, Plain, IndexX, IndexY, Ind, IndX, IndY };

struct Parsed {
    Shape shape = Shape::None;
    std::string expr;
};

// Splits "expr,X" into ("expr", 'X'); index = 0 if there is no top-level ",X"/",Y".
char splitIndex(const std::string& s, std::string& base) {
    const std::vector<std::string> parts = splitOperands(s);
    if (parts.size() == 2) {
        const std::string r = upper(trim(parts[1]));
        if (r == "X" || r == "Y") {
            base = trim(parts[0]);
            return r[0];
        }
    }
    base = s;
    return 0;
}

Parsed parseOperand(const std::string& text, bool isJmp) {
    Parsed p;
    const std::string s = trim(text);
    if (s.empty()) return p;
    if (upper(s) == "A") {
        p.shape = Shape::Acc;
        return p;
    }
    if (s[0] == '#') {
        p.shape = Shape::Imm;
        p.expr = trim(s.substr(1));
        return p;
    }
    if (s[0] == '(') {
        const std::size_t close = matching(s, 0);
        if (close == s.size() - 1) {
            const std::string inner = trim(s.substr(1, close - 1));
            std::string base;
            if (splitIndex(inner, base) == 'X') {
                p.shape = Shape::IndX;
                p.expr = base;
                return p;
            }
            if (isJmp) {
                p.shape = Shape::Ind;
                p.expr = inner;
                return p;
            }
            // "(1+2)" for a non-JMP: a plain expression in parentheses
        } else if (close != std::string::npos) {
            const std::string rest = trim(s.substr(close + 1));
            if (rest.size() >= 2 && rest[0] == ',' && upper(trim(rest.substr(1))) == "Y") {
                p.shape = Shape::IndY;
                p.expr = trim(s.substr(1, close - 1));
                return p;
            }
        }
    }
    std::string base;
    const char idx = splitIndex(s, base);
    p.shape = idx == 'X' ? Shape::IndexX : idx == 'Y' ? Shape::IndexY : Shape::Plain;
    p.expr = base;
    return p;
}

const char* levelHint(OpKind kind) {
    switch (kind) {
        case OpKind::Illegal: return "an undocumented opcode (not allowed with .CPU 6502 / --no-illegal)";
        case OpKind::Unstable: return "an unstable undocumented opcode (needs .CPU 6510X / --unstable)";
        case OpKind::Jam: return "a JAM opcode (needs .CPU 6510X / --unstable)";
        default: return "";
    }
}

}  // namespace

std::vector<std::string> splitOperands(const std::string& text) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"') {
            quote = c;
        } else if (c == '\'' && i + 2 < text.size() && text[i + 2] == '\'') {
            cur += text.substr(i, 3);
            i += 2;
            continue;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
        } else if (c == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

EncodeResult encodeInstruction(const EncodeRequest& req, const ValueFn& value) {
    EncodeResult r;
    const std::string mn = canonicalMnemonic(req.mnemonic);
    if (!isMnemonic(mn)) {
        r.error = "unknown instruction '" + req.mnemonic + "'";
        return r;
    }
    const Parsed p = parseOperand(req.operand, mn == "JMP");

    // Emits `opcode` with operand bytes after checking the CPU level.
    auto emit = [&](Byte opcode, unsigned operandBytes, std::int64_t v) {
        const OpInfo& op = opInfo(opcode);
        if (!allowedAt(opcode, req.level)) {
            r.error = mn + " is " + levelHint(op.kind);
            return r;
        }
        r.bytes.push_back(opcode);
        if (operandBytes >= 1) r.bytes.push_back(static_cast<Byte>(v & 0xFF));
        if (operandBytes >= 2) r.bytes.push_back(static_cast<Byte>((v >> 8) & 0xFF));
        r.ok = true;
        return r;
    };
    auto fail = [&](const std::string& m) {
        r.error = m;
        return r;
    };
    auto modeMissing = [&](const char* what) {
        return fail(mn + " has no " + what + " addressing mode");
    };

    if (req.force && (p.shape == Shape::None || p.shape == Shape::Acc || p.shape == Shape::Imm ||
                      p.shape == Shape::Ind || p.shape == Shape::IndX || p.shape == Shape::IndY)) {
        return fail(std::string(".") + req.force + " only applies to zero page / absolute operands");
    }

    switch (p.shape) {
        case Shape::None:
            if (auto op = findOpcode(mn, Mode::Implied)) return emit(*op, 0, 0);
            if (auto op = findOpcode(mn, Mode::Accumulator)) return emit(*op, 0, 0);
            return fail(mn + " needs an operand");
        case Shape::Acc:
            if (auto op = findOpcode(mn, Mode::Accumulator)) return emit(*op, 0, 0);
            return fail(mn + " has no accumulator mode ('A' is a reserved name)");
        default:
            break;
    }
    if (findOpcode(mn, Mode::Implied) && !findOpcode(mn, Mode::Immediate) && !findOpcode(mn, Mode::ZeroPage) &&
        !findOpcode(mn, Mode::Absolute) && !findOpcode(mn, Mode::Relative)) {
        return fail(mn + " takes no operand");
    }

    const EncodeValue ev = value(p.expr);
    if (!ev.error.empty()) return fail(ev.error);
    const std::int64_t v = ev.value;

    switch (p.shape) {
        case Shape::Imm: {
            const auto op = findOpcode(mn, Mode::Immediate);
            if (!op) return modeMissing("immediate");
            if (!ev.undefined && (v < -128 || v > 255)) {
                return fail("immediate value " + std::to_string(v) + " out of range (-128..255)");
            }
            return emit(*op, 1, v);
        }
        case Shape::Ind: {
            const auto op = findOpcode(mn, Mode::Indirect);
            if (!op) return modeMissing("indirect");
            if (!ev.undefined && (v < 0 || v > 0xFFFF)) return fail("address out of range (0..$FFFF)");
            return emit(*op, 2, v);
        }
        case Shape::IndX:
        case Shape::IndY: {
            const Mode m = p.shape == Shape::IndX ? Mode::IndirectX : Mode::IndirectY;
            const auto op = findOpcode(mn, m);
            if (!op) return modeMissing(modeName(m));
            if (!ev.undefined && (v < 0 || v > 0xFF)) {
                return fail("zero-page pointer " + formatHex(static_cast<unsigned>(v & 0xFFFF), 4, NumberStyle::Dollar) +
                            " is not in the zero page");
            }
            return emit(*op, 1, v);
        }
        default:
            break;
    }

    // Plain or indexed: relative, zero page or absolute.
    if (p.shape == Shape::Plain) {
        if (auto op = findOpcode(mn, Mode::Relative)) {
            if (req.force) return fail("branches have no zero-page/absolute form");
            std::int64_t offset = 0;
            if (!ev.undefined) {
                if (v < 0 || v > 0xFFFF) return fail("branch target out of range (0..$FFFF)");
                offset = v - (static_cast<std::int64_t>(req.pc) + 2);
                // the program counter wraps at 64 KiB
                if (offset > 0x7FFF) offset -= 0x10000;
                if (offset < -0x8000) offset += 0x10000;
                if (offset < -128 || offset > 127) {
                    return fail("branch target too far (" + std::to_string(offset) + " bytes, allowed -128..127)");
                }
            }
            return emit(*op, 1, offset);
        }
    }
    const Mode zp = p.shape == Shape::IndexX ? Mode::ZeroPageX : p.shape == Shape::IndexY ? Mode::ZeroPageY : Mode::ZeroPage;
    const Mode ab = p.shape == Shape::IndexX ? Mode::AbsoluteX : p.shape == Shape::IndexY ? Mode::AbsoluteY : Mode::Absolute;
    const auto zpOp = findOpcode(mn, zp);
    const auto absOp = findOpcode(mn, ab);
    if (!zpOp && !absOp) return modeMissing(p.shape == Shape::Plain ? "zero page/absolute" : modeName(ab));
    if (!ev.undefined && (v < 0 || v > 0xFFFF)) return fail("address " + std::to_string(v) + " out of range (0..$FFFF)");

    const bool fits = ev.undefined || (v >= 0 && v <= 0xFF);
    bool useZp = false;
    if (req.force == 'B') {
        if (!zpOp) return modeMissing(modeName(zp));
        if (!fits) return fail("address " + formatHex(static_cast<unsigned>(v), 4, NumberStyle::Dollar) + " is not in the zero page (.B)");
        useZp = true;
    } else if (req.force == 'W') {
        if (!absOp) return modeMissing(modeName(ab));
    } else if (!absOp) {
        if (!fits) return fail(mn + " " + modeName(zp) + ": address " + formatHex(static_cast<unsigned>(v), 4, NumberStyle::Dollar) + " is not in the zero page");
        useZp = true;
    } else if (zpOp && req.hint == 0 && fits) {
        useZp = true;
    }
    if (useZp) {
        r.level = 0;
        return emit(*zpOp, 1, v);
    }
    r.level = zpOp ? 1 : 0;
    if (req.force == 'W') r.level = 0;  // forced forms never take part in growing
    return emit(*absOp, 2, v);
}

}  // namespace m6510
