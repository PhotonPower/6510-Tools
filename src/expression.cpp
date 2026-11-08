#include "m6510/expression.hpp"

#include <cctype>

namespace m6510 {

bool isSymbolStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }
bool isSymbolChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class Parser {
public:
    Parser(const std::string& s, const SymbolLookup& lookup, std::int64_t pc, bool hexDefault)
        : s_(s), lookup_(lookup), pc_(pc), hexDefault_(hexDefault) {}

    ExprResult run() {
        ExprResult r;
        skip();
        if (i_ >= s_.size()) {
            r.error = "empty expression";
            return r;
        }
        const std::int64_t v = parseTop();
        skip();
        if (!err_.empty()) {
            r.error = err_;
            return r;
        }
        if (i_ != s_.size()) {
            r.error = std::string("unexpected '") + s_[i_] + "' in expression";
            return r;
        }
        r.ok = true;
        r.value = v;
        r.undefined = undefined_;
        r.undefinedName = undefinedName_;
        return r;
    }

private:
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool peekIs(char c) {
        skip();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool peekIs2(char a, char b) {
        skip();
        return i_ + 1 < s_.size() && s_[i_] == a && s_[i_ + 1] == b;
    }
    void fail(const std::string& m) {
        if (err_.empty()) err_ = m;
    }

    // `<expr` / `>expr`: low / high byte of everything that follows (lowest precedence).
    std::int64_t parseTop() {
        if (++depth_ > 200) {
            fail("expression nested too deeply");
            return 0;
        }
        std::int64_t v = 0;
        if (peekIs('<') && !peekIs2('<', '<')) {
            ++i_;
            v = parseTop() & 0xFF;
        } else if (peekIs('>') && !peekIs2('>', '>')) {
            ++i_;
            v = (parseTop() >> 8) & 0xFF;
        } else {
            v = parseOr();
        }
        --depth_;
        return v;
    }

    std::int64_t parseOr() {
        std::int64_t v = parseXor();
        while (err_.empty() && peekIs('|')) {
            ++i_;
            v |= parseXor();
        }
        return v;
    }
    std::int64_t parseXor() {
        std::int64_t v = parseAnd();
        while (err_.empty() && peekIs('^')) {
            ++i_;
            v ^= parseAnd();
        }
        return v;
    }
    std::int64_t parseAnd() {
        std::int64_t v = parseShift();
        while (err_.empty() && peekIs('&')) {
            ++i_;
            v &= parseShift();
        }
        return v;
    }
    std::int64_t parseShift() {
        std::int64_t v = parseAdd();
        while (err_.empty()) {
            if (peekIs2('<', '<')) {
                i_ += 2;
                const std::int64_t n = parseAdd();
                v = (n < 0 || n > 63) ? 0 : static_cast<std::int64_t>(static_cast<std::uint64_t>(v) << n);
            } else if (peekIs2('>', '>')) {
                i_ += 2;
                const std::int64_t n = parseAdd();
                v = (n < 0 || n > 63) ? (v < 0 ? -1 : 0) : (v >> n);
            } else {
                break;
            }
        }
        return v;
    }
    std::int64_t parseAdd() {
        std::int64_t v = parseMul();
        while (err_.empty()) {
            if (peekIs('+')) {
                ++i_;
                v = static_cast<std::int64_t>(static_cast<std::uint64_t>(v) + static_cast<std::uint64_t>(parseMul()));
            } else if (peekIs('-')) {
                ++i_;
                v = static_cast<std::int64_t>(static_cast<std::uint64_t>(v) - static_cast<std::uint64_t>(parseMul()));
            } else {
                break;
            }
        }
        return v;
    }
    std::int64_t parseMul() {
        std::int64_t v = parseUnary();
        while (err_.empty()) {
            if (peekIs('*')) {
                ++i_;
                v = static_cast<std::int64_t>(static_cast<std::uint64_t>(v) * static_cast<std::uint64_t>(parseUnary()));
            } else if (peekIs('/') || peekIs('%')) {
                const char op = s_[i_++];
                const std::int64_t d = parseUnary();
                if (d == 0) {
                    fail("division by zero");
                    return 0;
                }
                if (d == -1) {
                    v = op == '/' ? static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(v)) : 0;
                } else {
                    v = op == '/' ? v / d : v % d;
                }
            } else {
                break;
            }
        }
        return v;
    }

    std::int64_t parseUnary() {
        if (++depth_ > 200) {
            fail("expression nested too deeply");
            return 0;
        }
        std::int64_t v = 0;
        if (peekIs('-')) {
            ++i_;
            v = static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(parseUnary()));
        } else if (peekIs('+')) {
            ++i_;
            v = parseUnary();
        } else if (peekIs('~')) {
            ++i_;
            v = ~parseUnary();
        } else {
            v = parsePrimary();
        }
        --depth_;
        return v;
    }

    static int digitValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 99;
    }

    std::int64_t parseRadix(int radix, const char* what) {
        std::uint64_t v = 0;
        std::size_t n = 0;
        while (i_ < s_.size() && digitValue(s_[i_]) < radix) {
            v = v * static_cast<unsigned>(radix) + static_cast<unsigned>(digitValue(s_[i_]));
            ++i_;
            ++n;
        }
        if (n == 0) fail(std::string("missing digits in ") + what + " number");
        if (n > 32) fail("number too long");
        return static_cast<std::int64_t>(v);
    }

    std::int64_t parseGroup(char close) {
        ++i_;
        const std::int64_t v = parseTop();
        if (!peekIs(close)) {
            fail(std::string("missing '") + close + "'");
            return 0;
        }
        ++i_;
        return v;
    }

    std::int64_t parsePrimary() {
        skip();
        if (i_ >= s_.size()) {
            fail("unexpected end of expression");
            return 0;
        }
        const char c = s_[i_];
        if (c == '(') return parseGroup(')');
        if (c == '[') return parseGroup(']');
        if (c == '*') {
            ++i_;
            return pc_;
        }
        if (c == '$') {
            ++i_;
            return parseRadix(16, "hexadecimal");
        }
        if (c == '%') {
            ++i_;
            return parseRadix(2, "binary");
        }
        if (c == '\'') {
            if (i_ + 2 < s_.size() && s_[i_ + 2] == '\'') {
                const auto v = static_cast<unsigned char>(s_[i_ + 1]);
                i_ += 3;
                return v;
            }
            fail("bad character constant");
            return 0;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (c == '0' && i_ + 1 < s_.size() && (s_[i_ + 1] == 'x' || s_[i_ + 1] == 'X')) {
                i_ += 2;
                return parseRadix(16, "hexadecimal");
            }
            if (c == '0' && i_ + 2 < s_.size() && (s_[i_ + 1] == 'b' || s_[i_ + 1] == 'B') &&
                (s_[i_ + 2] == '0' || s_[i_ + 2] == '1')) {
                i_ += 2;
                return parseRadix(2, "binary");
            }
            const std::size_t begin = i_;
            while (i_ < s_.size() && digitValue(s_[i_]) < 16) ++i_;
            std::string digits = s_.substr(begin, i_ - begin);
            if (i_ < s_.size() && (s_[i_] == 'h' || s_[i_] == 'H')) {  // 1Fh
                ++i_;
                return toNumber(digits, 16);
            }
            if (hexDefault_) {
                if (i_ < s_.size() && s_[i_] == '.') {  // 100. = decimal
                    ++i_;
                    if (digits.find_first_not_of("0123456789") != std::string::npos) {
                        fail("a number with a trailing '.' must be decimal");
                        return 0;
                    }
                    return toNumber(digits, 10);
                }
                return toNumber(digits, 16);
            }
            if (digits.find_first_not_of("0123456789") != std::string::npos) {
                fail("bad decimal number '" + digits + "' (hex needs $ or 0x)");
                return 0;
            }
            return toNumber(digits, 10);
        }
        if (isSymbolStart(c)) {
            std::string name;
            while (i_ < s_.size() && isSymbolChar(s_[i_])) name += s_[i_++];
            const std::string u = upper(name);
            if ((u == "LOW" || u == "HIGH") && peekIs('(')) {
                const std::int64_t v = parseGroup(')');
                return u == "LOW" ? (v & 0xFF) : ((v >> 8) & 0xFF);
            }
            std::int64_t v = 0;
            if (lookup_ && lookup_(name, v)) return v;
            if (hexDefault_ && name.size() <= 16 &&
                name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                return toNumber(name, 16);  // D020, FF, ...
            }
            if (!undefined_) undefinedName_ = name;
            undefined_ = true;
            return 0;
        }
        fail(std::string("unexpected '") + c + "' in expression");
        return 0;
    }

    std::int64_t toNumber(const std::string& digits, int radix) {
        if (digits.size() > 16) {
            fail("number too long");
            return 0;
        }
        return static_cast<std::int64_t>(std::stoull(digits, nullptr, radix));
    }

    const std::string& s_;
    const SymbolLookup& lookup_;
    std::int64_t pc_;
    bool hexDefault_;
    std::size_t i_ = 0;
    int depth_ = 0;
    std::string err_;
    bool undefined_ = false;
    std::string undefinedName_;
};

}  // namespace

ExprResult ExpressionEvaluator::evaluate(const std::string& text) const {
    return Parser(text, lookup_, pc_, hexDefault_).run();
}

}  // namespace m6510
