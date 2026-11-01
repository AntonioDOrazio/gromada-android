#pragma once
// Abstract syntax tree of a script. Built by the Parser, annotated in place by the linker
// (variable slots, call targets, iff sites) and then treated as immutable.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "script/diagnostics.hpp"
#include "script/value.hpp"

namespace gromada::script {

struct FunctionDef;
struct BuiltinInfo;

enum class ExprKind : std::uint8_t {
    IntLiteral,   // intValue
    StrLiteral,   // text
    Variable,     // text = name
    Index,        // text = variable name, a = index           name[a]
    Call,         // text = callee name, args
    Unary,        // uop, a
    Binary,       // bop, a, b
    Logical,      // isAnd, a, b                               a && b, a || b
    Assign,       // a = target (Variable/Index), b = value; compound => bop applies
    IncDec,       // a = target; prefix, increment
    Conditional,  // a ? b : c
};

enum class UnaryOp : std::uint8_t { Negate, Plus, Not, BitNot };
enum class BinaryOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod, Shl, Shr, BitAnd, BitOr, BitXor, Less, LessEq, Greater, GreaterEq, Equal, NotEqual,
};

constexpr std::string_view binaryOpSpelling(BinaryOp op) {
    constexpr std::string_view names[] = {"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
                                          "<", "<=", ">", ">=", "==", "!="};
    return names[static_cast<int>(op)];
}

struct Expr {
    ExprKind kind;
    SourceLocation loc;
    std::int32_t intValue = 0;
    std::string text;
    UnaryOp uop = UnaryOp::Plus;
    BinaryOp bop = BinaryOp::Add;
    bool compound = false;  // Assign: a op= b
    bool isAnd = false;     // Logical
    bool prefix = false;    // IncDec
    bool increment = false; // IncDec
    std::unique_ptr<Expr> a, b, c;
    std::vector<std::unique_ptr<Expr>> args;

    // Link results.
    std::int32_t slot = -1;                 // Variable/Index: index of the global variable
    const FunctionDef* function = nullptr;  // Call: user function
    const BuiltinInfo* builtin = nullptr;   // Call: builtin (extern)

    Expr(ExprKind k, SourceLocation l) : kind{k}, loc{l} {}
};

enum class StmtKind : std::uint8_t { Empty, Expression, Block, If, Iff, For, While, DoWhile, Break, Continue, Return };

struct Stmt {
    StmtKind kind;
    SourceLocation loc;
    std::unique_ptr<Expr> expr;       // Expression, condition (If/Iff/For/While/DoWhile), Return value (optional)
    std::unique_ptr<Expr> init, step; // For (optional)
    std::unique_ptr<Stmt> body, elseBody;
    std::vector<std::unique_ptr<Stmt>> children; // Block
    std::uint32_t iffSite = 0;        // Iff: index of its once-flag (assigned by the linker)

    Stmt(StmtKind k, SourceLocation l) : kind{k}, loc{l} {}
};

struct VarDeclarator {
    std::string name;
    SourceLocation loc;
    bool isArray = false;
    std::int32_t arraySize = 1;                  // number of elements (1 for scalars)
    std::unique_ptr<Expr> init;                  // scalar initializer
    std::vector<std::unique_ptr<Expr>> arrayInit; // {a, b, ...}
    bool hasInit() const { return init || !arrayInit.empty(); }
};

struct GlobalDecl {
    ValueType type;
    SourceLocation loc;
    std::vector<VarDeclarator> vars;
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

struct FunctionDef {
    std::string name;
    SourceLocation loc;
    std::vector<ParamDecl> params;
    std::unique_ptr<Stmt> body;
    std::vector<std::int32_t> paramSlots; // link result: global variable of each parameter
};

struct ExternDecl {
    std::string name;
    SourceLocation loc;
    std::vector<ParamDecl> params;
    std::int32_t id = 0;
};

// One top-level item, in source order.
struct TopLevelItem {
    enum class Kind : std::uint8_t { Global, Function, Extern } kind;
    std::size_t index; // into ProgramAst::globals / functions / externs
};

struct ProgramAst {
    std::vector<GlobalDecl> globals;
    std::vector<std::unique_ptr<FunctionDef>> functions;
    std::vector<ExternDecl> externs;
    std::vector<TopLevelItem> order;
};

} // namespace gromada::script
