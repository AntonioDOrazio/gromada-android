#pragma once
// Recursive-descent parser: preprocessed tokens -> ProgramAst.
//
// Top level:
//   extern Name(params) ID;                       builtin declaration (params: [int|string] [name])
//   int a, b = 1, c[3] = {1, 2, 3};               global variables
//   string s, t[4];
//   Name(int a; string b) { ... }                 user function (parameters separated by ';' or ',')
//   Name(...);                                    forward declaration (accepted, ignored)
// Statements: blocks, if/else, iff, for, while, do-while, break, continue, return [(expr)],
// empty statement, expression statement. Expressions: the C operator set and precedence
// (assignment and compound assignment, ?:, || && | ^ & == != < <= > >= << >> + - * / %,
// unary - + ! ~ ++ --, postfix ++ --, indexing, calls).
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "script/ast.hpp"
#include "script/lexer.hpp"

namespace gromada::script {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : toks_{tokens} {}

    ProgramAst parseProgram() {
        ProgramAst prog;
        while (cur().kind != TokenKind::End) {
            if (cur().is(Punct::Semicolon)) { next(); continue; } // stray ';' between items
            if (cur().isIdent("extern")) {
                prog.order.push_back({TopLevelItem::Kind::Extern, prog.externs.size()});
                prog.externs.push_back(parseExtern());
                continue;
            }
            // [type] Name ( ... )  => function;  type declarators ;  => globals
            const bool typed = isTypeKeyword(cur());
            const std::size_t nameAt = typed ? 1 : 0;
            if (peek(nameAt).kind == TokenKind::Identifier && peek(nameAt + 1).is(Punct::LParen)) {
                if (typed) next();
                auto fn = parseFunction();
                if (fn) {
                    prog.order.push_back({TopLevelItem::Kind::Function, prog.functions.size()});
                    prog.functions.push_back(std::move(fn));
                }
                continue;
            }
            if (typed) {
                prog.order.push_back({TopLevelItem::Kind::Global, prog.globals.size()});
                prog.globals.push_back(parseGlobalDecl());
                continue;
            }
            fail(cur().loc, "expected a declaration, a function or 'extern' at top level, found " + cur().describe());
        }
        return prog;
    }

    // Parses a single expression (used by tools and tests).
    std::unique_ptr<Expr> parseStandaloneExpression() {
        auto e = parseExpression();
        if (cur().kind != TokenKind::End) fail(cur().loc, "unexpected " + cur().describe() + " after expression");
        return e;
    }

    // Constant folding used for array sizes; nullopt if `e` is not an integer constant expression.
    static std::optional<std::int32_t> foldConstant(const Expr& e) {
        using namespace intops;
        switch (e.kind) {
            case ExprKind::IntLiteral: return e.intValue;
            case ExprKind::Unary: {
                const auto v = foldConstant(*e.a);
                if (!v) return std::nullopt;
                switch (e.uop) {
                    case UnaryOp::Negate: return neg(*v);
                    case UnaryOp::Plus: return *v;
                    case UnaryOp::Not: return *v == 0 ? 1 : 0;
                    case UnaryOp::BitNot: return ~*v;
                }
                return std::nullopt;
            }
            case ExprKind::Binary: {
                const auto l = foldConstant(*e.a), r = foldConstant(*e.b);
                if (!l || !r) return std::nullopt;
                return foldBinary(e.bop, *l, *r);
            }
            case ExprKind::Logical: {
                const auto l = foldConstant(*e.a), r = foldConstant(*e.b);
                if (!l || !r) return std::nullopt;
                return e.isAnd ? (*l && *r) : (*l || *r);
            }
            case ExprKind::Conditional: {
                const auto c = foldConstant(*e.a);
                if (!c) return std::nullopt;
                return foldConstant(*c ? *e.b : *e.c);
            }
            default: return std::nullopt;
        }
    }

    static std::int32_t foldBinary(BinaryOp op, std::int32_t l, std::int32_t r) {
        using namespace intops;
        switch (op) {
            case BinaryOp::Add: return add(l, r);
            case BinaryOp::Sub: return sub(l, r);
            case BinaryOp::Mul: return mul(l, r);
            case BinaryOp::Div: return div(l, r);
            case BinaryOp::Mod: return mod(l, r);
            case BinaryOp::Shl: return shl(l, r);
            case BinaryOp::Shr: return shr(l, r);
            case BinaryOp::BitAnd: return l & r;
            case BinaryOp::BitOr: return l | r;
            case BinaryOp::BitXor: return l ^ r;
            case BinaryOp::Less: return l < r;
            case BinaryOp::LessEq: return l <= r;
            case BinaryOp::Greater: return l > r;
            case BinaryOp::GreaterEq: return l >= r;
            case BinaryOp::Equal: return l == r;
            case BinaryOp::NotEqual: return l != r;
        }
        return 0;
    }

    static bool isKeyword(std::string_view s) {
        return s == "if" || s == "else" || s == "iff" || s == "for" || s == "while" || s == "do" || s == "return" ||
               s == "break" || s == "continue" || s == "int" || s == "string" || s == "extern";
    }

private:
    // ---- token helpers ----
    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(std::size_t ahead) const {
        const std::size_t p = pos_ + ahead;
        return p < toks_.size() ? toks_[p] : toks_.back();
    }
    const Token& next() {
        const Token& t = toks_[pos_];
        if (t.kind != TokenKind::End) ++pos_;
        return t;
    }
    bool accept(Punct p) {
        if (!cur().is(p)) return false;
        next();
        return true;
    }
    const Token& expect(Punct p, std::string_view context) {
        if (!cur().is(p))
            fail(cur().loc, std::format("expected '{}' {}, found {}", punctSpelling(p), context, cur().describe()));
        return next();
    }
    std::string expectIdentifier(std::string_view context) {
        if (cur().kind != TokenKind::Identifier || isKeyword(cur().text))
            fail(cur().loc, std::format("expected an identifier {}, found {}", context, cur().describe()));
        return next().text;
    }
    static bool isTypeKeyword(const Token& t) { return t.isIdent("int") || t.isIdent("string"); }

    [[noreturn]] static void fail(const SourceLocation& loc, std::string msg) {
        throw ScriptError(ScriptError::Phase::Parse, loc, std::move(msg));
    }

    // ---- declarations ----
    ExternDecl parseExtern() {
        ExternDecl d;
        d.loc = next().loc; // 'extern'
        if (isTypeKeyword(cur())) next(); // optional return type
        d.loc = cur().loc;
        d.name = expectIdentifier("after 'extern'");
        d.params = parseParams(/*forExtern=*/true);
        bool negative = accept(Punct::Minus);
        if (cur().kind != TokenKind::Number) fail(cur().loc, "expected the numeric id of extern '" + d.name + "'");
        d.id = negative ? intops::neg(next().number) : next().number;
        expect(Punct::Semicolon, "after extern declaration");
        return d;
    }

    // ( [type] [name] {(,|;) [type] [name]} )
    std::vector<ParamDecl> parseParams(bool forExtern) {
        std::vector<ParamDecl> params;
        expect(Punct::LParen, "to open the parameter list");
        if (accept(Punct::RParen)) return params;
        for (;;) {
            ParamDecl p;
            p.loc = cur().loc;
            if (isTypeKeyword(cur())) {
                p.type = next().isIdent("int") ? ParamType::Int : ParamType::String;
                if (cur().kind == TokenKind::Identifier && !isKeyword(cur().text)) p.name = next().text;
            } else if (cur().kind == TokenKind::Identifier && !isKeyword(cur().text)) {
                // Untyped parameter: `variable` in `extern FWrite(int file,variable)`.
                p.type = forExtern ? ParamType::Any : ParamType::Int;
                p.name = next().text;
            } else {
                fail(cur().loc, "expected a parameter, found " + cur().describe());
            }
            if (!forExtern && p.name.empty()) fail(p.loc, "function parameters need a name");
            params.push_back(std::move(p));
            if (accept(Punct::RParen)) return params;
            if (!accept(Punct::Comma) && !accept(Punct::Semicolon))
                fail(cur().loc, "expected ',', ';' or ')' in parameter list, found " + cur().describe());
        }
    }

    std::unique_ptr<FunctionDef> parseFunction() {
        auto fn = std::make_unique<FunctionDef>();
        fn->loc = cur().loc;
        fn->name = expectIdentifier("as function name");
        fn->params = parseParams(/*forExtern=*/false);
        if (accept(Punct::Semicolon)) return nullptr; // forward declaration
        if (!cur().is(Punct::LBrace)) fail(cur().loc, "expected '{' to start the body of '" + fn->name + "', found " + cur().describe());
        fn->body = parseBlock();
        return fn;
    }

    GlobalDecl parseGlobalDecl() {
        GlobalDecl d;
        d.loc = cur().loc;
        d.type = next().isIdent("int") ? ValueType::Int : ValueType::String;
        for (;;) {
            VarDeclarator v;
            v.loc = cur().loc;
            v.name = expectIdentifier("in declaration");
            if (accept(Punct::LBracket)) {
                v.isArray = true;
                v.arraySize = -1;
                if (!cur().is(Punct::RBracket)) {
                    const auto sizeLoc = cur().loc;
                    auto sizeExpr = parseConditional();
                    const auto size = foldConstant(*sizeExpr);
                    if (!size) fail(sizeLoc, "array size must be an integer constant");
                    if (*size <= 0) fail(sizeLoc, "array size must be positive");
                    v.arraySize = *size;
                }
                expect(Punct::RBracket, "after array size");
            }
            if (accept(Punct::Assign)) {
                if (v.isArray) {
                    expect(Punct::LBrace, "to start the array initializer");
                    if (!cur().is(Punct::RBrace)) {
                        for (;;) {
                            v.arrayInit.push_back(parseAssignment());
                            if (!accept(Punct::Comma)) break;
                            if (cur().is(Punct::RBrace)) break; // trailing comma
                        }
                    }
                    expect(Punct::RBrace, "to close the array initializer");
                    if (v.arraySize < 0) v.arraySize = static_cast<std::int32_t>(v.arrayInit.size());
                    if (v.arrayInit.size() > static_cast<std::size_t>(v.arraySize))
                        fail(v.loc, std::format("too many initializers for '{}[{}]'", v.name, v.arraySize));
                } else {
                    v.init = parseAssignment();
                }
            }
            if (v.isArray && v.arraySize <= 0) fail(v.loc, "array '" + v.name + "' needs a size or an initializer");
            d.vars.push_back(std::move(v));
            if (accept(Punct::Comma)) continue;
            expect(Punct::Semicolon, "after declaration");
            return d;
        }
    }

    // ---- statements ----
    std::unique_ptr<Stmt> parseBlock() {
        auto s = std::make_unique<Stmt>(StmtKind::Block, expect(Punct::LBrace, "").loc);
        while (!cur().is(Punct::RBrace)) {
            if (cur().kind == TokenKind::End) fail(s->loc, "unterminated block: missing '}'");
            s->children.push_back(parseStatement());
        }
        next();
        return s;
    }

    std::unique_ptr<Expr> parseParenCondition(std::string_view keyword) {
        expect(Punct::LParen, std::format("after '{}'", keyword));
        auto e = parseExpression();
        expect(Punct::RParen, std::format("to close the '{}' condition", keyword));
        return e;
    }

    std::unique_ptr<Stmt> parseStatement() {
        const Token& t = cur();
        if (t.is(Punct::LBrace)) return parseBlock();
        if (t.is(Punct::Semicolon)) return std::make_unique<Stmt>(StmtKind::Empty, next().loc);
        if (t.kind == TokenKind::Identifier) {
            const std::string& kw = t.text;
            if (kw == "if" || kw == "iff") {
                const bool once = kw == "iff";
                auto s = std::make_unique<Stmt>(once ? StmtKind::Iff : StmtKind::If, next().loc);
                s->expr = parseParenCondition(kw);
                s->body = parseStatement();
                if (cur().isIdent("else")) {
                    if (once) fail(cur().loc, "'else' after 'iff' is not supported (its meaning is unknown)");
                    next();
                    s->elseBody = parseStatement();
                }
                return s;
            }
            if (kw == "else") fail(t.loc, "'else' without a matching 'if'");
            if (kw == "while") {
                auto s = std::make_unique<Stmt>(StmtKind::While, next().loc);
                s->expr = parseParenCondition(kw);
                s->body = parseStatement();
                return s;
            }
            if (kw == "do") {
                auto s = std::make_unique<Stmt>(StmtKind::DoWhile, next().loc);
                s->body = parseStatement();
                if (!cur().isIdent("while")) fail(cur().loc, "expected 'while' after 'do' body");
                next();
                s->expr = parseParenCondition("while");
                expect(Punct::Semicolon, "after do-while");
                return s;
            }
            if (kw == "for") {
                auto s = std::make_unique<Stmt>(StmtKind::For, next().loc);
                expect(Punct::LParen, "after 'for'");
                if (!cur().is(Punct::Semicolon)) s->init = parseExpression();
                expect(Punct::Semicolon, "after 'for' initialization");
                if (!cur().is(Punct::Semicolon)) s->expr = parseExpression();
                expect(Punct::Semicolon, "after 'for' condition");
                if (!cur().is(Punct::RParen)) s->step = parseExpression();
                expect(Punct::RParen, "to close 'for'");
                s->body = parseStatement();
                return s;
            }
            if (kw == "break" || kw == "continue") {
                auto s = std::make_unique<Stmt>(kw == "break" ? StmtKind::Break : StmtKind::Continue, next().loc);
                expect(Punct::Semicolon, std::format("after '{}'", kw));
                return s;
            }
            if (kw == "return") {
                auto s = std::make_unique<Stmt>(StmtKind::Return, next().loc);
                if (!cur().is(Punct::Semicolon)) s->expr = parseExpression();
                expect(Punct::Semicolon, "after 'return'");
                return s;
            }
            if (kw == "int" || kw == "string")
                fail(t.loc, "variable declarations inside functions are not supported; declare globals at top level");
            if (kw == "extern") fail(t.loc, "'extern' is only allowed at top level");
        }
        auto s = std::make_unique<Stmt>(StmtKind::Expression, t.loc);
        s->expr = parseExpression();
        expect(Punct::Semicolon, "after expression");
        return s;
    }

    // ---- expressions ----
    std::unique_ptr<Expr> parseExpression() { return parseAssignment(); }

    static bool isLvalue(const Expr& e) { return e.kind == ExprKind::Variable || e.kind == ExprKind::Index; }

    std::unique_ptr<Expr> parseAssignment() {
        auto lhs = parseConditional();
        const Token& t = cur();
        if (t.kind != TokenKind::Punct) return lhs;
        std::optional<BinaryOp> op;
        switch (t.punct) {
            case Punct::Assign: break;
            case Punct::PlusAssign: op = BinaryOp::Add; break;
            case Punct::MinusAssign: op = BinaryOp::Sub; break;
            case Punct::StarAssign: op = BinaryOp::Mul; break;
            case Punct::SlashAssign: op = BinaryOp::Div; break;
            case Punct::PercentAssign: op = BinaryOp::Mod; break;
            case Punct::AmpAssign: op = BinaryOp::BitAnd; break;
            case Punct::PipeAssign: op = BinaryOp::BitOr; break;
            case Punct::CaretAssign: op = BinaryOp::BitXor; break;
            case Punct::ShlAssign: op = BinaryOp::Shl; break;
            case Punct::ShrAssign: op = BinaryOp::Shr; break;
            default: return lhs;
        }
        // Game compatibility: `!unit=GetObject(...)` (levels/01.INI) is invalid C but the original
        // accepts it as `!(unit=GetObject(...))`: prefix unary operators whose operand is assigned
        // apply to the whole assignment.
        std::vector<std::unique_ptr<Expr>> unaryWrappers;
        while (lhs->kind == ExprKind::Unary && !isLvalue(*lhs)) {
            auto inner = std::move(lhs->a);
            unaryWrappers.push_back(std::move(lhs));
            lhs = std::move(inner);
        }
        if (!isLvalue(*lhs)) fail(t.loc, "left side of '" + t.text + "' is not a variable or an indexed variable");
        auto e = std::make_unique<Expr>(ExprKind::Assign, t.loc);
        next();
        e->compound = op.has_value();
        if (op) e->bop = *op;
        e->a = std::move(lhs);
        e->b = parseAssignment(); // right associative
        while (!unaryWrappers.empty()) { // innermost operator was pushed last
            auto u = std::move(unaryWrappers.back());
            unaryWrappers.pop_back();
            u->a = std::move(e);
            e = std::move(u);
        }
        return e;
    }

    std::unique_ptr<Expr> parseConditional() {
        auto cond = parseLogical(false);
        if (!cur().is(Punct::Question)) return cond;
        auto e = std::make_unique<Expr>(ExprKind::Conditional, next().loc);
        e->a = std::move(cond);
        e->b = parseExpression();
        expect(Punct::Colon, "in conditional expression");
        e->c = parseConditional();
        return e;
    }

    std::unique_ptr<Expr> parseLogical(bool andLevel) {
        auto lhs = andLevel ? parseBinary(0) : parseLogical(true);
        const Punct op = andLevel ? Punct::AndAnd : Punct::OrOr;
        while (cur().is(op)) {
            auto e = std::make_unique<Expr>(ExprKind::Logical, next().loc);
            e->isAnd = andLevel;
            e->a = std::move(lhs);
            e->b = andLevel ? parseBinary(0) : parseLogical(true);
            lhs = std::move(e);
        }
        return lhs;
    }

    // Binary levels from loosest to tightest: | ^ & equality relational shift additive multiplicative.
    static std::optional<BinaryOp> binaryAtLevel(const Token& t, int level) {
        if (t.kind != TokenKind::Punct) return std::nullopt;
        switch (level) {
            case 0: if (t.punct == Punct::Pipe) return BinaryOp::BitOr; break;
            case 1: if (t.punct == Punct::Caret) return BinaryOp::BitXor; break;
            case 2: if (t.punct == Punct::Amp) return BinaryOp::BitAnd; break;
            case 3:
                if (t.punct == Punct::EqEq) return BinaryOp::Equal;
                if (t.punct == Punct::NotEq) return BinaryOp::NotEqual;
                break;
            case 4:
                if (t.punct == Punct::Less) return BinaryOp::Less;
                if (t.punct == Punct::LessEq) return BinaryOp::LessEq;
                if (t.punct == Punct::Greater) return BinaryOp::Greater;
                if (t.punct == Punct::GreaterEq) return BinaryOp::GreaterEq;
                break;
            case 5:
                if (t.punct == Punct::Shl) return BinaryOp::Shl;
                if (t.punct == Punct::Shr) return BinaryOp::Shr;
                break;
            case 6:
                if (t.punct == Punct::Plus) return BinaryOp::Add;
                if (t.punct == Punct::Minus) return BinaryOp::Sub;
                break;
            case 7:
                if (t.punct == Punct::Star) return BinaryOp::Mul;
                if (t.punct == Punct::Slash) return BinaryOp::Div;
                if (t.punct == Punct::Percent) return BinaryOp::Mod;
                break;
        }
        return std::nullopt;
    }

    std::unique_ptr<Expr> parseBinary(int level) {
        if (level > 7) return parseUnary();
        auto lhs = parseBinary(level + 1);
        while (const auto op = binaryAtLevel(cur(), level)) {
            auto e = std::make_unique<Expr>(ExprKind::Binary, next().loc);
            e->bop = *op;
            e->a = std::move(lhs);
            e->b = parseBinary(level + 1);
            lhs = std::move(e);
        }
        return lhs;
    }

    std::unique_ptr<Expr> parseUnary() {
        const Token& t = cur();
        if (t.kind == TokenKind::Punct) {
            if (t.punct == Punct::PlusPlus || t.punct == Punct::MinusMinus) {
                auto e = std::make_unique<Expr>(ExprKind::IncDec, next().loc);
                e->prefix = true;
                e->increment = t.punct == Punct::PlusPlus;
                e->a = parseUnary();
                if (!isLvalue(*e->a)) fail(e->loc, "operand of prefix '" + t.text + "' is not a variable");
                return e;
            }
            std::optional<UnaryOp> op;
            if (t.punct == Punct::Minus) op = UnaryOp::Negate;
            else if (t.punct == Punct::Plus) op = UnaryOp::Plus;
            else if (t.punct == Punct::Bang) op = UnaryOp::Not;
            else if (t.punct == Punct::Tilde) op = UnaryOp::BitNot;
            if (op) {
                auto e = std::make_unique<Expr>(ExprKind::Unary, next().loc);
                e->uop = *op;
                e->a = parseUnary();
                return e;
            }
        }
        return parsePostfix();
    }

    std::unique_ptr<Expr> parsePostfix() {
        auto e = parsePrimary();
        for (;;) {
            if (cur().is(Punct::LBracket)) {
                if (e->kind != ExprKind::Variable) fail(cur().loc, "only variables can be indexed");
                auto idx = std::make_unique<Expr>(ExprKind::Index, e->loc);
                next();
                idx->text = std::move(e->text);
                idx->a = parseExpression();
                expect(Punct::RBracket, "to close the index");
                e = std::move(idx);
            } else if (cur().is(Punct::LParen)) {
                if (e->kind != ExprKind::Variable) fail(cur().loc, "only named functions can be called");
                auto call = std::make_unique<Expr>(ExprKind::Call, e->loc);
                next();
                call->text = std::move(e->text);
                if (!cur().is(Punct::RParen)) {
                    for (;;) {
                        call->args.push_back(parseAssignment());
                        if (!accept(Punct::Comma)) break;
                    }
                }
                expect(Punct::RParen, "to close the argument list of '" + call->text + "'");
                e = std::move(call);
            } else if (cur().is(Punct::PlusPlus) || cur().is(Punct::MinusMinus)) {
                if (!isLvalue(*e)) fail(cur().loc, "operand of postfix '" + cur().text + "' is not a variable");
                auto inc = std::make_unique<Expr>(ExprKind::IncDec, cur().loc);
                inc->prefix = false;
                inc->increment = next().punct == Punct::PlusPlus;
                inc->a = std::move(e);
                e = std::move(inc);
            } else {
                return e;
            }
        }
    }

    std::unique_ptr<Expr> parsePrimary() {
        const Token& t = cur();
        switch (t.kind) {
            case TokenKind::Number:
            case TokenKind::Char: {
                auto e = std::make_unique<Expr>(ExprKind::IntLiteral, t.loc);
                e->intValue = next().number;
                return e;
            }
            case TokenKind::String: {
                auto e = std::make_unique<Expr>(ExprKind::StrLiteral, t.loc);
                while (cur().kind == TokenKind::String) e->text += next().text; // "a" "b" concatenation
                return e;
            }
            case TokenKind::Identifier: {
                if (isKeyword(t.text)) fail(t.loc, "unexpected keyword '" + t.text + "' in expression");
                auto e = std::make_unique<Expr>(ExprKind::Variable, t.loc);
                e->text = next().text;
                return e;
            }
            case TokenKind::Punct:
                if (t.punct == Punct::LParen) {
                    next();
                    auto e = parseExpression();
                    expect(Punct::RParen, "to close parenthesized expression");
                    return e;
                }
                break;
            case TokenKind::End: fail(t.loc, "unexpected end of file in expression");
        }
        fail(t.loc, "unexpected " + t.describe() + " in expression");
    }

    const std::vector<Token>& toks_;
    std::size_t pos_ = 0;
};

} // namespace gromada::script
