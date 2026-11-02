#pragma once
// Bytecode of the original script VM (gromada.exe 0x41a790, docs/re/runtime.md §2.3-2.4).
//
// Operands: PUSHI imm32; PUSHS NUL-terminated bytes; JZ/JMP/IFF rel16 relative to the operand's
// own offset (target = operand offset + rel); CALL abs32; PUSHV/STORE/POSTINC/POSTDEC/PREINC/
// PREDEC one byte (cell index). Every other opcode has no operand; 64..139 are builtins.
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace gromada::script {

namespace op {
inline constexpr std::uint8_t PUSHI = 1, PUSHS = 2, ADD = 3, SUB = 4, MUL = 5, DIV = 6, MOD = 7, NEG = 8,
                              NOT = 9, XOR = 10, OR = 11, AND = 12, EQ = 13, LOR = 14, GT = 15, LT = 16,
                              GE = 17, LE = 18, LNOT = 19, NE = 20, LAND = 21, JZ = 22, PUSHV = 23,
                              STORE = 24, EOS = 25, JMP = 28, IFF = 29, CALL = 30, RET = 31,
                              POSTINC = 32, POSTDEC = 33, PREINC = 34, PREDEC = 35, INDEX = 36,
                              SHR = 37, SHL = 38;
} // namespace op

constexpr std::string_view opcodeName(std::uint8_t o) {
    constexpr std::string_view names[] = {
        "",      "PUSHI", "PUSHS", "ADD",   "SUB",   "MUL",     "DIV",     "MOD",    "NEG",    "NOT",
        "XOR",   "OR",    "AND",   "EQ",    "LOR",   "GT",      "LT",      "GE",     "LE",     "LNOT",
        "NE",    "LAND",  "JZ",    "PUSHV", "STORE", "EOS",     "",        "",       "JMP",    "IFF",
        "CALL",  "RET",   "POSTINC", "POSTDEC", "PREINC", "PREDEC", "INDEX", "SHR", "SHL"};
    return o < std::size(names) ? names[o] : std::string_view{};
}

inline std::uint32_t readU32(std::span<const std::uint8_t> code, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v |= std::uint32_t(at + i < code.size() ? code[at + i] : 0) << (8 * i);
    return v;
}
inline std::int16_t readI16(std::span<const std::uint8_t> code, std::size_t at) {
    const std::uint16_t lo = at < code.size() ? code[at] : 0, hi = at + 1 < code.size() ? code[at + 1] : 0;
    return static_cast<std::int16_t>(lo | hi << 8);
}

// Size in bytes of the instruction at `at` (opcode included).
inline std::size_t instructionSize(std::span<const std::uint8_t> code, std::size_t at) {
    switch (code[at]) {
        case op::PUSHI: case op::CALL: return 5;
        case op::PUSHS: {
            std::size_t e = at + 1;
            while (e < code.size() && code[e] != 0) ++e;
            return e + 1 - at;
        }
        case op::JZ: case op::JMP: case op::IFF: return 3;
        case op::PUSHV: case op::STORE: case op::POSTINC: case op::POSTDEC: case op::PREINC: case op::PREDEC: return 2;
        default: return 1;
    }
}

// One line per instruction: "offset  NAME operand". `nameOf(offset)` may label CALL targets and
// `cellName(index)` cell operands; `builtinName(op)` builtin opcodes.
template <class FuncName, class CellName, class BuiltinName>
std::string disassemble(std::span<const std::uint8_t> code, FuncName nameOf, CellName cellName, BuiltinName builtinName) {
    std::string out;
    for (std::size_t i = 0; i < code.size();) {
        const std::uint8_t o = code[i];
        const std::size_t n = instructionSize(code, i);
        std::string name{opcodeName(o)}, arg;
        switch (o) {
            case op::PUSHI: arg = std::to_string(std::int32_t(readU32(code, i + 1))); break;
            case op::PUSHS: {
                arg = "\"";
                for (std::size_t k = i + 1; k + 1 < i + n; ++k) {
                    const unsigned char c = code[k];
                    if (c == '"' || c == '\\') arg += '\\', arg += char(c);
                    else if (c == '\n') arg += "\\n";
                    else if (c == '\r') arg += "\\r";
                    else if (c < 0x20 || c >= 0x7F) arg += std::format("\\x{:02X}", c);
                    else arg += char(c);
                }
                arg += "\"";
                break;
            }
            case op::JZ: case op::JMP: case op::IFF:
                arg = std::format("-> {}", std::int64_t(i + 1) + readI16(code, i + 1));
                break;
            case op::CALL: {
                const std::uint32_t t = readU32(code, i + 1);
                arg = std::format("{} {}", t, nameOf(t));
                break;
            }
            case op::PUSHV: case op::STORE: case op::POSTINC: case op::POSTDEC: case op::PREINC: case op::PREDEC:
                arg = std::format("#{} {}", i + 1 < code.size() ? code[i + 1] : 0, cellName(i + 1 < code.size() ? code[i + 1] : 0));
                break;
            default:
                if (name.empty()) {
                    name = "BUILTIN";
                    arg = std::format("{} {}", o, builtinName(o));
                }
        }
        out += std::format("{:5}  {:<8} {}\n", i, name, arg);
        i += n;
    }
    return out;
}

} // namespace gromada::script
