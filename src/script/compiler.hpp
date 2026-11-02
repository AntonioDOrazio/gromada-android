#pragma once
// Compiler of the script language: a faithful port of the original recursive-descent compiler
// of gromada.exe (0x417cb0..0x41a790, docs/re/runtime.md §1-2), which turns the .INI source
// directly into the bytecode of a stack VM (bytecode.hpp). The reference replica is
// tools/re/runtime/gscript.py; tests/script_layout_compare.py checks that both produce the same
// bytes and the same cells for every game script.
//
// Behaviour of the original, quirks included:
//  - one pass, names must be declared before use; lookup order: functions (user and extern),
//    #define, variables, then the words int/string/extern/return, then a function definition
//    if '(' follows; the first declaration of a function or define name wins;
//  - every variable is a global cell, also parameters and declarations inside functions;
//    initializers are numeric literals applied once, at compile time;
//  - precedence: primary/unary, then * / %, then + -, then all comparisons and shifts at one
//    level, then ^ & && | || at one level; everything left-associative; no short circuit;
//  - `a[i] = e` evaluates e before i; a user call stores each argument into its parameter cell
//    (`STORE p; EOS`) before CALL; `return(e);` needs the parentheses;
//  - `iff` is an `if` whose IFF opcode patches the code (see interpreter.hpp).
#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "script/builtin_ids.hpp"
#include "script/builtins.hpp"
#include "script/bytecode.hpp"
#include "script/program.hpp"
#include "script/source.hpp"

namespace gromada::script {

struct CompileOptions {
    // Builtins declared before the script's first line, as if by `extern` (tests, fragments that
    // rely on HEADER.INI). The script's own declarations come after them.
    const BuiltinRegistry* predeclared = nullptr;
    // Fail like the original ("Not enough main() function") when main() is missing. Turn off to
    // compile include-only fragments (HEADER.INI, music.ini) for inspection.
    bool requireMain = true;
};

// The compiled program. Immutable; shared by every Interpreter running it.
struct CompiledProgram {
    std::vector<std::uint8_t> code;
    std::vector<CellDecl> cells;
    std::vector<GlobalVar> globals;
    std::vector<FunctionDef> functions;     // user functions, in definition order
    std::vector<DefineDef> defines;         // in definition order (the first of a name wins)
    BuiltinRegistry builtins;               // extern declarations, in declaration order
    std::vector<IffSite> iffSites;          // in source order
    std::vector<std::pair<std::uint32_t, SourceLocation>> lineMap; // statement starts, by code offset
    std::vector<CompileNote> notes;
    std::vector<std::string> files;         // every file read, the main file first
    std::uint32_t mainEntry = 0;
};

namespace detail {

class OriginalCompiler {
public:
    OriginalCompiler(SourceLoader& loader, SourceFiles& names, CompiledProgram& out, const CompileOptions& options)
        : loader_{loader}, names_{names}, p_{out}, options_{options} {}

    void compile(std::string_view mainPath) {
        p_.code = {op::CALL, 0, 0, 0, 0, op::JMP, 0x30, 0x75}; // CALL main ; JMP +30000
        if (options_.predeclared)
            for (const BuiltinInfo* b : options_.predeclared->declarations()) addExtern(*b);
        include(mainPath, /*isMain=*/true);
        p_.mainEntry = readU32(p_.code, 1);
        if (options_.requireMain && p_.mainEntry == 0)
            throw ScriptError(ScriptError::Phase::Link, {}, "LOGIC::Not enough main() function");
        finish();
    }

private:
    struct Func {
        std::string name;
        std::uint8_t op = 0;
        std::uint32_t entry = 0;
        std::int32_t paramBase = 0;
        std::int32_t userIndex = -1; // into p_.functions
    };

    // ---- character helpers (MSVC CRT ctype, ASCII part) ----
    static bool isSpace(unsigned c) { return c == ' ' || (c >= 9 && c <= 13); }
    static bool isDigit(unsigned c) { return c >= '0' && c <= '9'; }
    static bool isXDigit(unsigned c) { return isDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
    static bool isAlpha(unsigned c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
    static bool isAlnum(unsigned c) { return isAlpha(c) || isDigit(c); }

    unsigned cur(std::ptrdiff_t k = 0) const {
        const std::ptrdiff_t q = std::ptrdiff_t(pos_) + k;
        return q >= 0 && std::size_t(q) < buf_.size() ? static_cast<unsigned char>(buf_[std::size_t(q)]) : 0u;
    }
    bool atEnd() const { return pos_ >= buf_.size(); }

    SourceLocation here() const { return locationAt(pos_); }
    SourceLocation locationAt(std::size_t at) const {
        at = std::min(at, buf_.size());
        // Incremental line counting (positions are mostly increasing).
        if (at < lineCachePos_) {
            lineCachePos_ = 0;
            lineCacheLine_ = 1;
        }
        for (std::size_t i = lineCachePos_; i < at; ++i)
            if (buf_[i] == '\n') ++lineCacheLine_;
        lineCachePos_ = at;
        const std::size_t nl = at == 0 ? std::string::npos : buf_.rfind('\n', at - 1);
        const std::uint32_t col = std::uint32_t(nl == std::string::npos ? at + 1 : at - nl);
        return SourceLocation{file_, std::uint32_t(lineCacheLine_), col};
    }

    [[noreturn]] void fatal(const std::string& msg) const { throw ScriptError(ScriptError::Phase::Parse, here(), msg); }
    void note(std::string kind, std::string detail) { p_.notes.push_back({std::move(kind), here(), std::move(detail)}); }

    void emit(std::uint8_t b) { p_.code.push_back(b); }
    void emit32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) p_.code.push_back(std::uint8_t(v >> (8 * i)));
    }
    void put16(std::size_t at, std::int64_t v) {
        p_.code[at] = std::uint8_t(v & 0xFF);
        p_.code[at + 1] = std::uint8_t((v >> 8) & 0xFF);
    }

    // C sscanf("%i"): blanks, optional sign, 0x hex / leading-0 octal / decimal; nullopt if no
    // number. Wraps to 32 bits.
    std::optional<std::uint32_t> scanInt(std::size_t at) const {
        auto c = [&](std::size_t i) -> unsigned { return i < buf_.size() ? static_cast<unsigned char>(buf_[i]) : 0u; };
        std::size_t i = at;
        while (i < buf_.size() && isSpace(c(i))) ++i;
        bool negative = false;
        if (c(i) == '+' || c(i) == '-') negative = c(i++) == '-';
        std::uint32_t v = 0;
        if (c(i) == '0' && (c(i + 1) == 'x' || c(i + 1) == 'X') && isXDigit(c(i + 2))) {
            for (i += 2; isXDigit(c(i)); ++i) v = v * 16u + std::uint32_t(isDigit(c(i)) ? c(i) - '0' : (c(i) | 0x20) - 'a' + 10);
        } else if (c(i) == '0') {
            for (++i; c(i) >= '0' && c(i) <= '7'; ++i) v = v * 8u + (c(i) - '0');
        } else if (c(i) >= '1' && c(i) <= '9') {
            for (; isDigit(c(i)); ++i) v = v * 10u + (c(i) - '0');
        } else {
            return std::nullopt;
        }
        return negative ? 0u - v : v;
    }
    // After a number the original skips [0-9A-Fa-fx] (and '-' after initializers).
    void skipNumberChars(bool alsoMinus = false) {
        while (isXDigit(cur()) || cur() == 'x' || (alsoMinus && cur() == '-')) ++pos_;
    }

    // ---- lexical (0x418000, 0x4180b0) ----
    bool skipWs() { // returns true at the end of the buffer
        if (atEnd()) return true;
        for (;;) {
            const unsigned c = cur();
            if (c == '/' && cur(1) == '/') {
                for (;;) {
                    if (cur() == 13) break;
                    if (++pos_ >= buf_.size()) return true;
                    if (cur() == 10) break;
                }
                continue;
            }
            if (c == '/' && cur(1) == '*') {
                for (;;) {
                    if (cur(-1) == '/' && cur(-2) == '*') break;
                    if (++pos_ >= buf_.size()) return true;
                }
                continue;
            }
            if (!isSpace(c) && c != 0) return false;
            if (++pos_ >= buf_.size()) return true;
        }
    }

    std::string ident() {
        if (skipWs()) fatal("LOGIC::unened .ini file");
        std::string s;
        while (isAlnum(cur()) || cur() == '_') {
            if (s.size() >= 31) fatal("LOGIC::Long variable name");
            s.push_back(char(cur()));
            ++pos_;
        }
        return s;
    }

    // ---- expressions ----
    void primary() { // 0x418140
        std::uint8_t unary = 0;
        unsigned c = cur();
        if (c == '-' && cur(1) != '-') {
            ++pos_;
            unary = op::NEG;
        } else if (c == '~') {
            ++pos_;
            unary = op::NOT;
        } else if (c == '!') {
            ++pos_;
            unary = op::LNOT;
        }
        if (skipWs()) fatal("LOGIC::~!- not ended");
        std::uint8_t varop = op::PUSHV;
        if (cur() == '-' && cur(1) == '-') {
            varop = op::PREDEC;
            pos_ += 2;
        } else if (cur() == '+' && cur(1) == '+') {
            varop = op::PREINC;
            pos_ += 2;
        }
        if (skipWs()) fatal("LOGIC::Unended mnog");
        c = cur();
        if (isDigit(c)) {
            const auto v = scanInt(pos_);
            if (c == '0' && isDigit(cur(1))) note("octal-literal", "");
            emit(op::PUSHI);
            emit32(v.value_or(0));
            skipNumberChars();
        } else if (c == '"') {
            emit(op::PUSHS);
            ++pos_;
            bool escape = false;
            if (cur() != '"') {
                for (;;) {
                    if (atEnd()) fatal("LOGIC::Not enough second \"");
                    const unsigned ch = cur();
                    if (escape) {
                        escape = false;
                        emit(std::uint8_t(ch == 'n' ? 10 : ch == 'r' ? 13 : ch));
                    } else if (ch == '\\') {
                        escape = true;
                    } else {
                        emit(std::uint8_t(ch));
                    }
                    const unsigned next = cur(1);
                    ++pos_;
                    if (next == '"') break; // also after a backslash: \" ends the string
                }
            }
            ++pos_;
            emit(0);
        } else if (c == '\'') {
            std::int32_t v = std::int32_t(cur(1));
            if (v >= 0x80) v -= 0x100; // char is signed in MSVC
            emit(op::PUSHI);
            emit32(std::uint32_t(v));
            pos_ += 2;
            if (cur() != '\'') fatal("LOGIC::Enogth second '");
            ++pos_;
        } else if (isAlpha(c)) {
            identifier(varop);
        } else if (c == '(') {
            ++pos_;
            if (skipWs()) fatal("LOGIC::Not enough ) in end file");
            expr();
            if (cur() != ')') fatal("LOGIC::Not enough )");
            ++pos_;
        } else if (c == '#') {
            ++pos_;
            define();
        } else if (unary) {
            fatal("LOGIC::error:");
        }
        if (unary) emit(unary);
        skipWs();
    }

    void define() {
        const SourceLocation loc = here();
        if (ident() != "define") fatal("LOGIC::unknown preprocessor command");
        DefineDef d;
        d.name = ident();
        d.loc = loc;
        if (p_.defines.size() >= 256) fatal("LOGIC::Too many defines");
        if (skipWs()) fatal("LOGIC::Not enough define constant");
        if (cur() == '"') {
            ++pos_;
            while (cur() != '"') {
                if (atEnd()) fatal("LOGIC::Not enough second \"");
                d.text.push_back(char(cur()));
                ++pos_;
            }
            ++pos_;
            d.isString = true;
        } else {
            const auto v = scanInt(pos_);
            if (!v) note("define-not-number", d.name);
            d.value = std::int32_t(v.value_or(0));
            skipNumberChars();
        }
        for (const auto& old : p_.defines)
            if (old.name == d.name) {
                note("duplicate-define", d.name + " (the first definition wins)");
                break;
            }
        p_.defines.push_back(std::move(d));
    }

    void identifier(std::uint8_t varop) {
        const std::string name = ident();
        if (skipWs()) fatal("LOGIC::Unreferenced code");
        for (const auto& f : funcs_)
            if (f.name == name) return call(Func{f}); // copy: funcs_ may grow while parsing arguments
        for (const auto& d : p_.defines) {
            if (d.name != name) continue;
            if (d.isString) {
                emit(op::PUSHS);
                for (const char ch : d.text) emit(std::uint8_t(ch));
                emit(0);
            } else {
                emit(op::PUSHI);
                emit32(std::uint32_t(d.value));
            }
            return;
        }
        for (std::size_t vi = 0; vi < p_.cells.size(); ++vi)
            if (p_.cells[vi].name == name) return variable(vi, varop);
        if (name == "int" || name == "string") return declare(name == "string");
        if (name == "extern") return externDecl();
        if (name == "return") {
            if (cur() == '(') {
                ++pos_;
                if (skipWs()) fatal("LOGIC::Not enough ) after return(");
                expr();
                if (cur() != ')') fatal("LOGIC::Not enough ) after return(");
                ++pos_;
            }
            if (skipWs()) fatal("LOGIC::Not enough ; after return");
            emit(op::RET);
            return;
        }
        if (cur() == '(') return funcdef(name);
        fatal("LOGIC::Unknown name " + name);
    }

    void call(const Func f) {
        if (cur() != '(') fatal("LOGIC:: function without () " + f.name);
        ++pos_;
        std::int32_t argi = 0;
        for (;;) {
            if (skipWs()) fatal("LOGIC::Not enough ) in " + f.name);
            if (cur() != ')') {
                expr();
                if (f.op == op::CALL) {
                    emit(op::STORE);
                    emit(std::uint8_t((f.paramBase + argi) & 0xFF));
                    emit(op::EOS);
                }
            }
            const unsigned c = cur();
            ++pos_; // any character separates arguments
            ++argi;
            if (c == ')') break;
        }
        emit(f.op);
        if (f.op == op::CALL) emit32(f.entry);
    }

    void variable(std::size_t vi, std::uint8_t varop) {
        std::vector<std::uint8_t> index;
        bool indexed = false;
        if (cur() == '[') {
            const std::size_t save = p_.code.size();
            ++pos_;
            expr();
            emit(op::INDEX);
            index.assign(p_.code.begin() + std::ptrdiff_t(save), p_.code.end());
            p_.code.resize(save);
            indexed = true;
            const unsigned c = cur();
            ++pos_;
            if (c != ']') fatal("LOGIC::None ']'");
        }
        std::uint8_t o = varop;
        const unsigned c = cur();
        if (c == '=' && cur(1) != '=') {
            ++pos_;
            if (skipWs()) fatal("LOGIC::Not enough vyrag after =");
            expr();
            o = op::STORE;
        } else if (c == '+' && cur(1) == '+') {
            o = op::POSTINC;
            pos_ += 2;
        } else if (c == '-' && cur(1) == '-') {
            o = op::POSTDEC;
            pos_ += 2;
        }
        if (indexed) { // the index is evaluated after the assigned value
            p_.code.insert(p_.code.end(), index.begin(), index.end());
            const auto fl = p_.cells[vi].flags;
            if (o != op::STORE && (fl & cellflag::String) && !(fl & cellflag::Array))
                note("string-index-read", p_.cells[vi].name);
        }
        emit(o);
        emit(std::uint8_t(vi & 0xFF));
        if (vi > 255) note("var-index-overflow", p_.cells[vi].name);
    }

    void declare(bool isString) { // part of 0x418140
        for (;;) {
            const SourceLocation loc = here();
            const std::string name = ident();
            if (p_.cells.size() >= 256) fatal("LOGIC::Too many variables");
            for (const auto& v : p_.cells)
                if (v.name == name) fatal("LOGIC::" + name + " redefinition");
            std::uint32_t n = 1;
            if (cur() == '[') {
                ++pos_;
                skipWs();
                n = scanInt(pos_).value_or(0);
                skipNumberChars();
                skipWs();
                const unsigned c = cur();
                ++pos_;
                if (c != ']') fatal("LOGIC::None ']'");
                if (n > 2048) fatal("LOGIC::Too many variables"); // the original would run out of memory
            }
            const std::int32_t base = std::int32_t(p_.cells.size());
            if (n > 0) {
                GlobalVar g;
                g.name = name;
                g.type = isString ? ValueType::String : ValueType::Int;
                g.isArray = n > 1;
                g.size = std::int32_t(n);
                g.firstCell = base;
                g.loc = loc;
                g.origin = inParams_ ? GlobalVar::Origin::Parameter : GlobalVar::Origin::Declared;
                p_.globals.push_back(std::move(g));
            }
            for (std::uint32_t k = 0; k < n; ++k) {
                CellDecl cell;
                cell.name = name;
                cell.flags = std::uint8_t((isString ? cellflag::String : cellflag::Int) | (n > 1 ? cellflag::Array : 0));
                cell.variable = std::int32_t(p_.globals.size()) - 1;
                p_.cells.push_back(std::move(cell));
            }
            if (inFunction_) note("decl-in-function", name);
            skipWs();
            if (!isString && cur() == '=') {
                ++pos_;
                skipWs();
                auto setInit = [&](std::size_t cellIndex) {
                    if (cellIndex >= p_.cells.size()) return;
                    CellDecl& cell = p_.cells[cellIndex];
                    const auto v = scanInt(pos_);
                    cell.initialized = true;
                    cell.flags |= cellflag::Initialized;
                    cell.hasInitValue = v.has_value();
                    cell.initValue = v.value_or(0);
                    p_.globals.back().initialized = true;
                };
                if (n == 1) {
                    setInit(std::size_t(base));
                    skipNumberChars(/*alsoMinus=*/true);
                    skipWs();
                } else {
                    if (cur() != '{') fatal("LOGIC::Not enough { in initialized");
                    std::uint32_t i = 0;
                    for (;;) {
                        if (i >= n) fatal("LOGIC::too many initialized value");
                        ++pos_;
                        skipWs();
                        setInit(std::size_t(base) + i);
                        skipNumberChars(/*alsoMinus=*/true);
                        ++i;
                        skipWs();
                        if (cur() != ',') break;
                    }
                    if (i < n) note("partial-array-init", std::format("{} {}/{}", name, i, n));
                    const unsigned c = cur();
                    ++pos_;
                    if (c != '}') fatal("LOGIC::Not enough } in initialized");
                }
            }
            skipWs();
            const unsigned c = cur();
            ++pos_;
            if (c != ',') break;
        }
        --pos_;
    }

    void addExtern(const BuiltinInfo& b) {
        Func f;
        f.name = b.name;
        f.op = std::uint8_t(b.id & 0xFF);
        funcs_.push_back(f);
        BuiltinInfo info = b;
        info.id = f.op;
        p_.builtins.add(std::move(info));
    }

    static std::vector<ParamDecl> parseParamText(std::string_view text, const SourceLocation& loc) {
        std::vector<ParamDecl> params;
        auto trim = [](std::string_view s) {
            while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        };
        if (trim(text).empty()) return params;
        for (;;) {
            const auto sep = text.find_first_of(",;");
            const std::string_view item = trim(text.substr(0, sep));
            ParamDecl p;
            p.loc = loc;
            if (item.starts_with("int") && (item.size() == 3 || isSpace(static_cast<unsigned char>(item[3])))) {
                p.type = ParamType::Int;
                p.name = std::string{trim(item.substr(3))};
            } else if (item.starts_with("string") && (item.size() == 6 || isSpace(static_cast<unsigned char>(item[6])))) {
                p.type = ParamType::String;
                p.name = std::string{trim(item.substr(6))};
            } else {
                p.type = ParamType::Any;
                p.name = std::string{item};
            }
            params.push_back(std::move(p));
            if (sep == std::string_view::npos) break;
            text.remove_prefix(sep + 1);
        }
        return params;
    }

    // `extern Name(any text) ID;` (part of 0x418140): the text in parentheses is ignored.
    void externDecl() {
        if (funcs_.size() >= 128) fatal("LOGIC::Too many extern function");
        const SourceLocation loc = here();
        const std::string name = ident();
        const std::size_t pstart = pos_;
        unsigned c = cur();
        ++pos_;
        if (c != ')') {
            for (;;) {
                if (atEnd()) fatal("LOGIC::Not enough extern function code");
                c = cur();
                ++pos_;
                if (c == ')') break;
            }
        }
        const std::size_t pend = pos_ - 1;
        if (skipWs() || !isDigit(cur())) fatal("LOGIC::Not enough extern function code");
        const std::uint8_t id = std::uint8_t(scanInt(pos_).value_or(0) & 0xFF);
        skipNumberChars();
        for (const auto& f : funcs_)
            if (f.name == name) {
                note("duplicate-extern", std::format("{} {} (the first declaration wins)", name, id));
                break;
            }
        BuiltinInfo info;
        info.id = id;
        info.name = name;
        info.loc = loc;
        if (pstart < pend) info.params = parseParamText(std::string_view{buf_}.substr(pstart + 1, pend - pstart - 1), loc);
        addExtern(info);
    }

    void funcdef(const std::string& name) {
        if (funcs_.size() >= 128) fatal("LOGIC::Too many extern function");
        for (const auto& f : funcs_)
            if (f.name == name) fatal("LOGIC::" + name + "() redefinition");
        FunctionDef def;
        def.name = name;
        def.loc = here();
        def.paramBase = std::int32_t(p_.cells.size());
        Func f;
        f.name = name;
        f.paramBase = def.paramBase;
        f.userIndex = std::int32_t(p_.functions.size());
        funcs_.push_back(f);
        p_.functions.push_back(def);
        const std::size_t fi = funcs_.size() - 1;
        const std::size_t ui = std::size_t(f.userIndex);
        ++pos_; // '('
        inParams_ = true;
        for (;;) {
            if (skipWs()) fatal("LOGIC::Not enough ) in " + name);
            expr();
            if (funcs_[fi].paramBase == std::int32_t(p_.cells.size()) && cur() != ')') fatal("LOGIC::Incorrect declare " + name);
            const unsigned c = cur();
            ++pos_;
            if (c == ')') break;
        }
        inParams_ = false;
        if (skipWs()) fatal("LOGIC::Not enough function body " + name);
        funcs_[fi].op = op::CALL;
        funcs_[fi].entry = std::uint32_t(p_.code.size());
        {
            FunctionDef& fd = p_.functions[ui];
            fd.entry = funcs_[fi].entry;
            fd.paramCount = std::int32_t(p_.cells.size()) - fd.paramBase;
            for (std::int32_t k = fd.paramBase; k < std::int32_t(p_.cells.size()); ++k) {
                const CellDecl& cell = p_.cells[std::size_t(k)];
                if (k > fd.paramBase && cell.name == p_.cells[std::size_t(k) - 1].name) continue;
                ParamDecl pd;
                pd.type = (cell.flags & cellflag::String) ? ParamType::String : ParamType::Int;
                pd.name = cell.name;
                fd.params.push_back(std::move(pd));
            }
        }
        if (name == "main")
            for (int i = 0; i < 4; ++i) p_.code[std::size_t(1 + i)] = std::uint8_t(funcs_[fi].entry >> (8 * i));
        if (cur() != '{') fatal("LOGIC::Not enough function body " + name);
        ++pos_;
        inFunction_ = true;
        if (cur() != '}') {
            for (;;) {
                if (skipWs()) fatal("LOGIC::Not enough } in " + name);
                if (stmt(nullptr)) fatal("LOGIC::Not enough } in " + name);
                if (cur() == '}') break;
            }
        }
        inFunction_ = false;
        // The original overwrites the closing '}' with ';' so that the caller emits an EOS.
        buf_[pos_] = ';';
        emit(op::RET);
        p_.functions[ui].end = std::uint32_t(p_.code.size());
    }

    using Level = std::pair<std::string_view, std::uint8_t>;
    template <std::size_t N>
    void binlevel(void (OriginalCompiler::*sub)(), const Level (&ops)[N]) {
        (this->*sub)();
        for (;;) {
            bool matched = false;
            for (const auto& [tok, o] : ops) {
                if (std::string_view{buf_}.substr(std::min(pos_, buf_.size()), tok.size()) != tok) continue;
                if (tok.size() == 1 && (tok[0] == '&' || tok[0] == '|' || tok[0] == '>' || tok[0] == '<') &&
                    (cur(1) == unsigned(tok[0]) || cur(1) == '='))
                    continue;
                pos_ += tok.size();
                if (skipWs()) fatal("LOGIC::Not enough second operand");
                (this->*sub)();
                emit(o);
                matched = true;
                break;
            }
            if (!matched) break;
        }
        skipWs();
    }
    void mnog() { // 0x4196d0
        static constexpr Level ops[] = {{"*", op::MUL}, {"/", op::DIV}, {"%", op::MOD}};
        binlevel(&OriginalCompiler::primary, ops);
    }
    void slag() { // 0x4197d0
        static constexpr Level ops[] = {{"+", op::ADD}, {"-", op::SUB}};
        binlevel(&OriginalCompiler::mnog, ops);
    }
    void compare() { // 0x419880
        static constexpr Level ops[] = {{">=", op::GE}, {">>", op::SHR}, {">", op::GT}, {"<=", op::LE},
                                        {"<<", op::SHL}, {"<", op::LT},  {"==", op::EQ}, {"!=", op::NE}};
        binlevel(&OriginalCompiler::slag, ops);
    }
    void expr() { // 0x419b40
        static constexpr Level ops[] = {{"^", op::XOR}, {"&&", op::LAND}, {"&", op::AND}, {"||", op::LOR}, {"|", op::OR}};
        binlevel(&OriginalCompiler::compare, ops);
    }

    // ---- statements (0x419cf0); returns true at the end of the buffer ----
    bool stmt(std::vector<std::size_t>* breaks) {
        if (skipWs()) fatal("LOGIC::unened .ini file");
        const std::size_t start = pos_;
        const SourceLocation loc = here();
        p_.lineMap.emplace_back(std::uint32_t(p_.code.size()), loc);
        const std::string w = ident();
        if (w == "if" || w == "iff") {
            const bool isIff = w == "iff";
            const std::uint32_t conditionStart = std::uint32_t(p_.code.size());
            if (skipWs()) fatal("LOGIC::Unended 'if'");
            if (cur() != '(') fatal("LOGIC::'if' without (");
            ++pos_;
            if (skipWs()) fatal("LOGIC::Not enough ) in 'if'");
            expr();
            if (cur() != ')') fatal("LOGIC::'if' without )");
            ++pos_;
            emit(isIff ? op::IFF : op::JZ);
            const std::size_t operand = p_.code.size();
            emit(0);
            emit(0);
            if (isIff) p_.iffSites.push_back({conditionStart, std::uint32_t(operand - 1), loc, false});
            if (skipWs()) fatal("LOGIC::Not enough ; in 'if'");
            stmt(breaks);
            const std::size_t after = pos_;
            put16(operand, std::int64_t(p_.code.size()) - std::int64_t(operand));
            if (ident() == "else") {
                put16(operand, readI16(p_.code, operand) + 3);
                emit(op::JMP);
                const std::size_t op2 = p_.code.size();
                emit(0);
                emit(0);
                if (isIff) note("iff-else", "");
                if (skipWs()) fatal("LOGIC::Not enough ; in 'else'");
                stmt(breaks);
                put16(op2, std::int64_t(p_.code.size()) - std::int64_t(op2));
                return skipWs();
            }
            pos_ = after;
            return skipWs();
        }
        if (w == "while") {
            std::vector<std::size_t> myBreaks;
            if (skipWs()) fatal("LOGIC::Unended 'while'");
            if (cur() != '(') fatal("LOGIC::'while' without (");
            ++pos_;
            if (skipWs()) fatal("LOGIC::Not enough ) in 'while'");
            const std::size_t top = p_.code.size();
            expr();
            const unsigned c = cur();
            ++pos_;
            if (c != ')') fatal("LOGIC::'while' without )");
            emit(op::JZ);
            const std::size_t operand = p_.code.size();
            emit(0);
            emit(0);
            if (skipWs()) fatal("LOGIC::Not enough ; in 'while'");
            stmt(&myBreaks);
            emit(op::JMP);
            const std::size_t j = p_.code.size();
            emit(0);
            emit(0);
            put16(j, std::int64_t(top) - std::int64_t(j));
            const std::size_t end = p_.code.size();
            put16(operand, std::int64_t(end) - std::int64_t(operand));
            for (const std::size_t b : myBreaks) put16(b, std::int64_t(end) - std::int64_t(b));
            return skipWs();
        }
        if (w == "for") {
            std::vector<std::size_t> myBreaks;
            if (skipWs()) fatal("LOGIC::Unended 'for'");
            unsigned c = cur();
            ++pos_;
            if (c != '(') fatal("LOGIC::'for' without (");
            if (skipWs()) fatal("LOGIC::Not enough ) in 'for'");
            expr(); // the initialization's value stays on the stack until the condition's JZ
            c = cur();
            ++pos_;
            if (c != ';') fatal("LOGIC::Not enough first ; in 'for'");
            const std::size_t top = p_.code.size();
            if (skipWs()) fatal("LOGIC::Not enough ) in 'for'");
            expr();
            c = cur();
            ++pos_;
            if (c != ';') fatal("LOGIC::Not enough second ; in 'for'");
            if (skipWs()) fatal("LOGIC::Not enough ) in 'for'");
            emit(op::JZ);
            const std::size_t operand = p_.code.size();
            emit(0);
            emit(0);
            const std::size_t incr = pos_;
            for (int depth = 0;;) {
                if (atEnd()) fatal("LOGIC::Not enough ) in 'for'");
                c = cur();
                if (c == '(') ++depth;
                else if (c == ')') --depth;
                ++pos_;
                if (depth < 0) break;
            }
            stmt(&myBreaks);
            const std::size_t after = pos_;
            pos_ = incr; // the increment is compiled after the body
            expr();
            pos_ = after;
            emit(op::EOS);
            emit(op::JMP);
            const std::size_t j = p_.code.size();
            emit(0);
            emit(0);
            put16(j, std::int64_t(top) - std::int64_t(j));
            const std::size_t end = p_.code.size();
            put16(operand, std::int64_t(end) - std::int64_t(operand));
            for (const std::size_t b : myBreaks) put16(b, std::int64_t(end) - std::int64_t(b));
            return skipWs();
        }
        if (w == "break") {
            if (skipWs()) fatal("LOGIC::Unended 'break'");
            if (cur() != ';') fatal("LOGIC::'break' without ;");
            ++pos_;
            if (!breaks) fatal("LOGIC::'break' without loop");
            emit(op::JMP);
            breaks->push_back(p_.code.size());
            emit(0);
            emit(0);
            return skipWs();
        }
        pos_ = start;
        const unsigned c = cur();
        if (c == '{') {
            ++pos_;
            if (cur() != '}') { // `{ }` with blanks inside is an error, like in the original
                for (;;) {
                    if (skipWs()) fatal("LOGIC::Not enough }");
                    if (stmt(breaks)) fatal("LOGIC::Not enough }");
                    if (cur() == '}') break;
                }
            }
            ++pos_;
            return skipWs();
        }
        if (c == '#' && std::string_view{buf_}.substr(pos_ + 1, 7) == "include") {
            ++pos_;
            ident();
            if (skipWs() || cur() != '"') fatal("LOGIC::Not enough include file name");
            ++pos_;
            std::string name;
            while (cur() != '"') {
                if (atEnd()) fatal("LOGIC::Not enough second \"");
                name.push_back(char(cur()));
                ++pos_;
            }
            ++pos_;
            include(name, /*isMain=*/false);
            return skipWs();
        }
        if (c == '#') {
            expr();
            return skipWs();
        }
        expr();
        if (cur() != ';') fatal("LOGIC::Not enough ;");
        ++pos_;
        emit(op::EOS);
        return skipWs();
    }

    void include(std::string_view name, bool isMain) {
        if (depth_ >= 16) fatal("LOGIC::#include nested too deeply");
        auto source = loader_.load(name, isMain ? std::string_view{} : std::string_view{key_});
        if (!source) {
            if (isMain) throw ScriptError(ScriptError::Phase::Load, {}, "LOGIC::Can't open file " + std::string{name});
            fatal("LOGIC::Can't open file " + std::string{name});
        }
        std::string savedBuf = std::move(buf_), savedKey = std::move(key_);
        const std::size_t savedPos = pos_, savedCachePos = lineCachePos_, savedCacheLine = lineCacheLine_;
        const std::string* savedFile = file_;
        buf_ = std::move(source->text);
        key_ = std::move(source->key);
        file_ = names_.intern(source->name);
        p_.files.push_back(source->name);
        pos_ = 0;
        lineCachePos_ = 0;
        lineCacheLine_ = 1;
        ++depth_;
        if (!skipWs())
            while (!stmt(nullptr)) {}
        --depth_;
        buf_ = std::move(savedBuf);
        key_ = std::move(savedKey);
        file_ = savedFile;
        pos_ = savedPos;
        lineCachePos_ = savedCachePos;
        lineCacheLine_ = savedCacheLine;
    }

    void finish() {
        // iff sites whose statement start is a JMP target (other than the initial JMP +30000).
        std::vector<bool> jumpTarget(p_.code.size() + 1, false);
        for (std::size_t i = 0; i < p_.code.size(); i += instructionSize(p_.code, i)) {
            if (p_.code[i] != op::JMP || i == 5) continue;
            const std::int64_t t = std::int64_t(i + 1) + readI16(p_.code, i + 1);
            if (t >= 0 && std::size_t(t) < jumpTarget.size()) jumpTarget[std::size_t(t)] = true;
        }
        for (auto& s : p_.iffSites) {
            s.reachableByJump = jumpTarget[s.conditionStart];
            if (s.reachableByJump) p_.notes.push_back({"iff-reached-by-jump", s.loc, ""});
        }
    }

    SourceLoader& loader_;
    SourceFiles& names_;
    CompiledProgram& p_;
    const CompileOptions& options_;
    std::vector<Func> funcs_;
    std::string buf_, key_;
    const std::string* file_ = nullptr;
    std::size_t pos_ = 0;
    mutable std::size_t lineCachePos_ = 0, lineCacheLine_ = 1;
    int depth_ = 0;
    bool inFunction_ = false, inParams_ = false;
};

} // namespace detail

class Script {
public:
    // Compiles `mainPath` (loaded through `loader`, which also serves #include).
    static Script compile(SourceLoader& loader, std::string_view mainPath, const CompileOptions& options = {}) {
        auto impl = std::make_shared<Impl>();
        detail::OriginalCompiler{loader, impl->names, impl->program, options}.compile(mainPath);
        return Script{std::move(impl)};
    }

    // Compiles source text held in memory; #include is served by `includes` if given.
    static Script compileText(std::string_view text, const CompileOptions& options = {},
                              std::string_view name = "<memory>", SourceLoader* includes = nullptr) {
        struct Overlay : SourceLoader {
            std::string name, text;
            SourceLoader* next = nullptr;
            std::optional<LoadedSource> load(std::string_view path, std::string_view from) override {
                if (from.empty() && path == name) return LoadedSource{name, "\x01main:" + name, text};
                if (next) return next->load(path, from);
                return std::nullopt;
            }
        } overlay;
        overlay.name = std::string{name};
        overlay.text = std::string{text};
        overlay.next = includes;
        return compile(overlay, name, options);
    }

    const CompiledProgram& program() const { return impl_->program; }
    const std::vector<std::uint8_t>& code() const { return impl_->program.code; }
    // Cells declared by the script (= number of globals of the original, LOGIC+0x291c).
    const std::vector<CellDecl>& cells() const { return impl_->program.cells; }
    std::size_t cellCount() const { return impl_->program.cells.size(); }
    // Variables (an array is one variable spanning several cells), in declaration order.
    const std::vector<GlobalVar>& globals() const { return impl_->program.globals; }
    const GlobalVar* findGlobalVar(std::string_view name) const {
        for (const auto& g : impl_->program.globals)
            if (g.name == name) return &g;
        return nullptr;
    }
    // Index into globals() of the variable `name`.
    std::optional<std::int32_t> findGlobal(std::string_view name) const {
        const auto& gs = impl_->program.globals;
        for (std::size_t i = 0; i < gs.size(); ++i)
            if (gs[i].name == name) return std::int32_t(i);
        return std::nullopt;
    }
    const FunctionDef* findFunction(std::string_view name) const {
        for (const auto& f : impl_->program.functions)
            if (f.name == name) return &f;
        return nullptr;
    }
    const FunctionDef* functionAt(std::uint32_t entry) const {
        for (const auto& f : impl_->program.functions)
            if (f.entry == entry) return &f;
        return nullptr;
    }
    const std::vector<FunctionDef>& functions() const { return impl_->program.functions; }
    const BuiltinRegistry& builtins() const { return impl_->program.builtins; }
    const std::vector<DefineDef>& defines() const { return impl_->program.defines; }
    // The definition a use of `name` expands to (the first one).
    const DefineDef* findDefine(std::string_view name) const {
        for (const auto& d : impl_->program.defines)
            if (d.name == name) return &d;
        return nullptr;
    }
    std::uint32_t iffSiteCount() const { return std::uint32_t(impl_->program.iffSites.size()); }
    const std::vector<IffSite>& iffSites() const { return impl_->program.iffSites; }
    const std::vector<CompileNote>& notes() const { return impl_->program.notes; }
    // Notes as text ("file:line:col: note: kind detail").
    std::vector<std::string> warnings() const {
        std::vector<std::string> r;
        for (const auto& n : impl_->program.notes)
            r.push_back(n.loc.str() + ": note: " + n.kind + (n.detail.empty() ? "" : " " + n.detail));
        return r;
    }
    const std::vector<std::string>& files() const { return impl_->program.files; }
    std::uint32_t mainEntry() const { return impl_->program.mainEntry; }

    // Source location of the statement containing code offset `pc`.
    SourceLocation locationOf(std::uint32_t pc) const {
        const auto& m = impl_->program.lineMap;
        const auto it = std::upper_bound(m.begin(), m.end(), pc, [](std::uint32_t v, const auto& e) { return v < e.first; });
        if (it == m.begin()) return {};
        return std::prev(it)->second;
    }

    // Disassembly of `code` (default: the compiled code) with function, cell and builtin names.
    std::string disassemble(std::span<const std::uint8_t> code = {}) const {
        const auto& p = impl_->program;
        if (code.empty()) code = p.code;
        return script::disassemble(
            code,
            [&](std::uint32_t t) {
                const FunctionDef* f = functionAt(t);
                return f ? f->name : std::string{};
            },
            [&](std::uint8_t c) { return c < p.cells.size() ? p.cells[c].name : std::string{"?"}; },
            [&](std::uint8_t o) {
                const BuiltinInfo* b = p.builtins.findById(o);
                return b ? b->name : std::string{"?"};
            });
    }

private:
    struct Impl {
        SourceFiles names;
        CompiledProgram program;
    };
    explicit Script(std::shared_ptr<Impl> impl) : impl_{std::move(impl)} {}
    std::shared_ptr<Impl> impl_;
};

} // namespace gromada::script
