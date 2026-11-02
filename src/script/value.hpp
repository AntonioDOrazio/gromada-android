#pragma once
// Runtime values of the script language: 32-bit integers and byte strings.
//
// Strings are raw bytes in the script's code page (CP866/CP1251/CP1252 depending on the file);
// the interpreter never re-encodes them. Integers follow the 32-bit two's complement arithmetic
// of the original x86 executable (wrap-around, no undefined behaviour). Values are what builtins
// receive and return; inside the interpreter they live in cells (see interpreter.hpp).
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace gromada::script {

enum class ValueType : std::uint8_t { Int, String };

constexpr std::string_view typeName(ValueType t) { return t == ValueType::Int ? "int" : "string"; }

// Wrap-around integer helpers (the script language has no undefined behaviour).
namespace intops {
constexpr std::int32_t wrap(std::uint32_t v) { return static_cast<std::int32_t>(v); }
constexpr std::int32_t add(std::int32_t a, std::int32_t b) { return wrap(std::uint32_t(a) + std::uint32_t(b)); }
constexpr std::int32_t sub(std::int32_t a, std::int32_t b) { return wrap(std::uint32_t(a) - std::uint32_t(b)); }
constexpr std::int32_t mul(std::int32_t a, std::int32_t b) { return wrap(std::uint32_t(a) * std::uint32_t(b)); }
constexpr std::int32_t neg(std::int32_t a) { return wrap(0u - std::uint32_t(a)); }
// The original's DIV opcode: x/0 = 0x0FFFFFFF; otherwise x86 `idiv` (truncation toward zero).
// INT_MIN/-1 raises a CPU exception in the original: callers decide (see ExecConfig).
inline constexpr std::int32_t kDivByZero = 0x0FFFFFFF;
constexpr bool divideFaults(std::int32_t a, std::int32_t b) { return b == 0 || (b == -1 && a == INT32_MIN); }
// Shift counts are masked to 5 bits like the x86 SHL/SAR instructions; >> is arithmetic.
constexpr std::int32_t shl(std::int32_t a, std::int32_t b) { return wrap(std::uint32_t(a) << (std::uint32_t(b) & 31u)); }
constexpr std::int32_t shr(std::int32_t a, std::int32_t b) { return a >> (std::uint32_t(b) & 31u); }
} // namespace intops

// Converts a string to an integer like the MSVC CRT atoi() used by the original: leading
// isspace() characters, optional sign, decimal digits; anything else stops; no digits -> 0.
// Wraps on overflow (the CRT accumulates without checks).
constexpr std::int32_t stringToInt(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    std::uint32_t v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10u + std::uint32_t(s[i] - '0');
    return intops::wrap(negative ? 0u - v : v);
}

inline std::string intToString(std::int32_t v) {
    char buf[16];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, end);
}

class Value {
public:
    Value() : v_{std::int32_t{0}} {}
    Value(std::int32_t i) : v_{i} {}
    Value(std::string s) : v_{std::move(s)} {}
    Value(std::string_view s) : v_{std::string{s}} {}
    Value(const char* s) : v_{std::string{s}} {}

    ValueType type() const { return v_.index() == 0 ? ValueType::Int : ValueType::String; }
    bool isInt() const { return v_.index() == 0; }
    bool isString() const { return v_.index() == 1; }

    // Exact accessors (precondition: matching type).
    std::int32_t intValue() const { return std::get<0>(v_); }
    const std::string& stringValue() const { return std::get<1>(v_); }
    std::string& stringValue() { return std::get<1>(v_); }

    // Converting accessors: string -> int uses stringToInt (atoi), int -> string is decimal.
    std::int32_t toInt() const { return isInt() ? intValue() : stringToInt(stringValue()); }
    std::string toString() const { return isString() ? stringValue() : intToString(intValue()); }
    Value convertedTo(ValueType t) const {
        if (t == type()) return *this;
        return t == ValueType::Int ? Value{toInt()} : Value{toString()};
    }

    // Truth value in conditions: like the original, a string is converted with atoi
    // ("12" is true, "abc" and "0" are false).
    bool truthy() const { return toInt() != 0; }

    friend bool operator==(const Value&, const Value&) = default;

    // Human-readable form for logs: integers in decimal, strings quoted with C escapes
    // (bytes outside printable ASCII as \xNN, so the output is code-page neutral).
    std::string repr() const {
        if (isInt()) return intToString(intValue());
        std::string out = "\"";
        for (unsigned char c : stringValue()) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20 || c >= 0x7F) out += std::format("\\x{:02X}", c);
                    else out += char(c);
            }
        }
        return out + "\"";
    }

private:
    std::variant<std::int32_t, std::string> v_;
};

} // namespace gromada::script
