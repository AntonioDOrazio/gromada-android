#pragma once
// Description of a compiled script: variable cells, functions, builtins (externs), defines and
// iff sites. Produced by the compiler (compiler.hpp), read by the interpreter and by tools.
//
// The layout follows the original compiler of gromada.exe (docs/re/runtime.md §1.2, §2.6):
// every variable element is a "cell" (4-byte value + 1-byte flags) in one array shared with the
// evaluation stack; arrays are consecutive cells with the same name; function parameters are
// ordinary global cells declared when the function header is compiled.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostics.hpp"
#include "script/value.hpp"

namespace gromada::script {

// Flags byte of a cell (the original's values).
namespace cellflag {
inline constexpr std::uint8_t String = 1;
inline constexpr std::uint8_t Int = 2;
inline constexpr std::uint8_t Array = 4;
inline constexpr std::uint8_t Initialized = 8;
} // namespace cellflag

// One variable cell as declared by the script.
struct CellDecl {
    std::string name;
    std::uint8_t flags = cellflag::Int;
    // Compile-time initializer (`int a=5;`, `int v[3]={1,2};`). `initialized` without a value
    // happens when the original's sscanf("%i") fails: the flag is set, the value is left alone.
    bool initialized = false;
    bool hasInitValue = false;
    std::uint32_t initValue = 0;
    std::int32_t variable = 0; // index into Script::globals()
};

// A variable (scalar or array) = one or more consecutive cells with the same name.
struct GlobalVar {
    enum class Origin : std::uint8_t { Declared, Parameter };
    std::string name;
    ValueType type = ValueType::Int;
    bool isArray = false;      // flag 4: declared with a size > 1
    std::int32_t size = 1;     // number of cells
    std::int32_t firstCell = 0;
    SourceLocation loc;
    Origin origin = Origin::Declared;
    bool initialized = false;  // at least one cell has a compile-time initializer
    bool hasInitializer() const { return initialized; }
};

enum class ParamType : std::uint8_t { Int, String, Any };

constexpr std::string_view paramTypeName(ParamType t) {
    return t == ParamType::Int ? "int" : t == ParamType::String ? "string" : "any";
}

struct ParamDecl {
    ParamType type = ParamType::Int;
    std::string name; // may be empty for extern parameters
    SourceLocation loc;
};

// A user function. Its parameters are the global cells [paramBase, paramBase + paramCount).
struct FunctionDef {
    std::string name;
    SourceLocation loc;
    std::uint32_t entry = 0;      // code offset of the body
    std::uint32_t end = 0;        // code offset after the final RET
    std::int32_t paramBase = 0;   // first cell declared by the parameter list
    std::int32_t paramCount = 0;  // cells declared by the parameter list
    std::vector<ParamDecl> params;
};

// `#define NAME value`: an integer (sscanf "%i") or a string between quotes (no escapes).
struct DefineDef {
    std::string name;
    bool isString = false;
    std::string text;
    std::int32_t value = 0;
    SourceLocation loc;
};

// An `iff` statement. `conditionStart` is the code offset where its statement begins (the
// condition's code), `opcodeOffset` the offset of the IFF instruction itself.
struct IffSite {
    std::uint32_t conditionStart = 0;
    std::uint32_t opcodeOffset = 0;
    SourceLocation loc;
    // The condition's first instruction is the target of a JMP (end of an if/else branch,
    // `break`): the original then patches that JMP instead of the iff, so the iff can fire again
    // when reached another way (docs/re/runtime.md §1.7).
    bool reachableByJump = false;
};

// Findings of the compiler about constructs that the original executes in a surprising way.
struct CompileNote {
    std::string kind;   // partial-array-init, duplicate-extern, duplicate-define, string-index-read,
                        // octal-literal, decl-in-function, define-not-number, iff-reached-by-jump,
                        // var-index-overflow, iff-else
    SourceLocation loc;
    std::string detail;
};

} // namespace gromada::script
