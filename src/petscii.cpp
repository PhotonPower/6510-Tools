#include "m6510/petscii.hpp"

#include <array>

#include "m6510/format.hpp"

namespace m6510 {

namespace {

constexpr Byte kSysToken = 0x9E;

// clang-format off
constexpr std::array<const char*, 76> kTokens = {
    "END", "FOR", "NEXT", "DATA", "INPUT#", "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF", "RESTORE", "GOSUB",
    "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "VERIFY", "DEF", "POKE", "PRINT#", "PRINT", "CONT", "LIST",
    "CLR", "CMD", "SYS", "OPEN", "CLOSE", "GET", "NEW", "TAB(", "TO", "FN", "SPC(", "THEN", "NOT", "STEP", "+", "-",
    "*", "/", "^", "AND", "OR", ">", "=", "<", "SGN", "INT", "ABS", "USR", "FRE", "POS", "SQR", "RND", "LOG", "EXP",
    "COS", "SIN", "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$", "GO",
};
// clang-format on

const char* controlName(Byte b) {
    switch (b) {
        case 0x05: return "WHT";
        case 0x0D: return "RETURN";
        case 0x0E: return "LOWER";
        case 0x11: return "DOWN";
        case 0x12: return "RVS ON";
        case 0x13: return "HOME";
        case 0x14: return "DEL";
        case 0x1C: return "RED";
        case 0x1D: return "RIGHT";
        case 0x1E: return "GRN";
        case 0x1F: return "BLU";
        case 0x81: return "ORANGE";
        case 0x8E: return "UPPER";
        case 0x90: return "BLK";
        case 0x91: return "UP";
        case 0x92: return "RVS OFF";
        case 0x93: return "CLR";
        case 0x94: return "INST";
        case 0x95: return "BROWN";
        case 0x96: return "LT RED";
        case 0x97: return "DK GRAY";
        case 0x98: return "GRAY";
        case 0x99: return "LT GREEN";
        case 0x9A: return "LT BLUE";
        case 0x9B: return "LT GRAY";
        case 0x9C: return "PUR";
        case 0x9D: return "LEFT";
        case 0x9E: return "YEL";
        case 0x9F: return "CYN";
        default: return nullptr;
    }
}

}  // namespace

std::optional<Byte> asciiToPetscii(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    if (c >= 'a' && c <= 'z') return static_cast<Byte>(c - 'a' + 0x41);
    if (c >= 'A' && c <= 'Z') return static_cast<Byte>(c - 'A' + 0xC1);
    if ((c >= 0x20 && c <= 0x40) || c == '[' || c == ']') return static_cast<Byte>(c);
    if (c == '^') return Byte{0x5E};  // up arrow
    return std::nullopt;
}

std::optional<Byte> asciiToScreenCode(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '@') return Byte{0};
    if (c >= 'a' && c <= 'z') return static_cast<Byte>(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z') return static_cast<Byte>(c - 'A' + 0x41);
    if (c == '[') return Byte{0x1B};
    if (c == ']') return Byte{0x1D};
    if (c == '^') return Byte{0x1E};
    if (c >= 0x20 && c <= 0x3F) return static_cast<Byte>(c);
    return std::nullopt;
}

std::string petsciiToText(Byte b) {
    if ((b >= 0x20 && b <= 0x5B) || b == 0x5D) return std::string(1, static_cast<char>(b));
    if (b == 0x5E) return "^";
    if (const char* n = controlName(b)) return std::string("{") + n + "}";
    return "{" + formatHex(b, 2, NumberStyle::Dollar) + "}";
}

std::string basicToken(Byte token) {
    if (token >= 0x80 && token < 0x80 + kTokens.size()) return kTokens[token - 0x80];
    if (token == 0xFF) return "{PI}";
    return "";
}

std::vector<BasicLine> detokenizeBasic(const Memory& memory, Address start, std::string* error, std::size_t maxLines) {
    std::vector<BasicLine> lines;
    unsigned a = start;
    auto setError = [&](const std::string& m) {
        if (error) *error = m;
    };
    if (error) error->clear();
    while (lines.size() < maxLines) {
        if (a + 1 > 0xFFFF) {
            setError("program runs past the end of memory");
            break;
        }
        const Word link = memory.read16(static_cast<Address>(a));
        if (link == 0) break;
        if (link <= a + 4) {
            setError("broken line link at " + formatHex(a, 4, NumberStyle::Dollar));
            break;
        }
        BasicLine line;
        line.address = static_cast<Address>(a);
        line.next = link;
        line.number = memory.read16(static_cast<Address>(a + 2));
        bool quote = false, rem = false, ended = false;
        for (unsigned p = a + 4; p < link && p <= 0xFFFF; ++p) {
            const Byte b = memory.read8(static_cast<Address>(p));
            if (b == 0) {
                ended = true;
                break;
            }
            if (b == '"') quote = !quote;
            if (!quote && !rem && (b >= 0x80) && !basicToken(b).empty()) {
                line.text += basicToken(b);
                if (b == 0x8F) rem = true;
            } else {
                line.text += petsciiToText(b);
            }
        }
        if (!ended) {
            setError("line " + std::to_string(line.number) + " has no end marker before its link");
            lines.push_back(line);
            break;
        }
        lines.push_back(line);
        a = link;
    }
    return lines;
}

std::vector<Byte> makeBasicStub(Address at, unsigned line, unsigned target) {
    const std::string digits = std::to_string(target);
    const unsigned next = at + 2 + 2 + 1 + static_cast<unsigned>(digits.size()) + 1;
    std::vector<Byte> b;
    b.push_back(static_cast<Byte>(next & 0xFF));
    b.push_back(static_cast<Byte>((next >> 8) & 0xFF));
    b.push_back(static_cast<Byte>(line & 0xFF));
    b.push_back(static_cast<Byte>((line >> 8) & 0xFF));
    b.push_back(kSysToken);
    for (char d : digits) b.push_back(static_cast<Byte>(d));
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    return b;
}

std::optional<SysStub> parseSysStub(const Memory& memory, Address at) {
    if (memory.read8(static_cast<Address>(at + 4)) != kSysToken) return std::nullopt;
    unsigned target = 0;
    std::size_t n = 0;
    for (unsigned p = at + 5U; n < 5 && p <= 0xFFFF; ++p, ++n) {
        const Byte b = memory.read8(static_cast<Address>(p));
        if (b < '0' || b > '9') break;
        target = target * 10 + (b - '0');
    }
    if (n == 0 || target > 0xFFFF) return std::nullopt;
    SysStub s;
    s.line = memory.read16(static_cast<Address>(at + 2));
    s.target = static_cast<Address>(target);
    const std::vector<Byte> expect = makeBasicStub(at, s.line, target);
    if (at + expect.size() > 0x10000) return std::nullopt;
    if (memory.readBytes(at, expect.size()) != expect) return std::nullopt;
    s.length = expect.size();
    return s;
}

std::optional<Address> findSysTarget(const Memory& memory, Address start) {
    const Word link = memory.read16(start);
    if (link == 0 || link <= start + 4) return std::nullopt;
    unsigned p = start + 4U;
    const unsigned end = link;
    while (p < end && memory.read8(static_cast<Address>(p)) != kSysToken) {
        if (memory.read8(static_cast<Address>(p)) == 0) return std::nullopt;
        ++p;
    }
    if (p >= end) return std::nullopt;
    ++p;
    while (p < end && memory.read8(static_cast<Address>(p)) == ' ') ++p;
    unsigned target = 0;
    std::size_t n = 0;
    while (p < end && n < 6) {
        const Byte b = memory.read8(static_cast<Address>(p));
        if (b < '0' || b > '9') break;
        target = target * 10 + (b - '0');
        ++p;
        ++n;
    }
    if (n == 0 || target > 0xFFFF) return std::nullopt;
    return static_cast<Address>(target);
}

char screenCodeToChar(Byte code) {
    const auto sc = static_cast<Byte>(code & 0x7F);
    if (sc == 0) return '@';
    if (sc <= 26) return static_cast<char>('A' + sc - 1);
    if (sc == 27) return '[';
    if (sc == 29) return ']';
    if (sc >= 32 && sc < 64) return static_cast<char>(sc);
    return '.';
}

}  // namespace m6510
