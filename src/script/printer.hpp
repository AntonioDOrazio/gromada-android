#pragma once
// Prints an AST back as source, with every compound expression parenthesized and every
// statement body braced, so that the structure chosen by the parser (precedence, dangling else)
// is explicit. Used by tools/script_run --dump and by round-trip tests.
#include <string>

#include "script/ast.hpp"

namespace gromada::script {

class AstPrinter {
public:
    static std::string expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLiteral: // negative only via wrap-around (0xFFFFFFFF): keep it re-parsable
                return e.intValue < 0 ? "(" + std::to_string(e.intValue) + ")" : std::to_string(e.intValue);
            case ExprKind::StrLiteral: return Value{e.text}.repr();
            case ExprKind::Variable: return e.text;
            case ExprKind::Index: return e.text + "[" + expr(*e.a) + "]";
            case ExprKind::Call: {
                std::string s = e.text + "(";
                for (std::size_t i = 0; i < e.args.size(); ++i) s += (i ? ", " : "") + expr(*e.args[i]);
                return s + ")";
            }
            case ExprKind::Unary: {
                constexpr const char* ops[] = {"-", "+", "!", "~"};
                return std::string{"("} + ops[static_cast<int>(e.uop)] + expr(*e.a) + ")";
            }
            case ExprKind::Binary:
                return "(" + expr(*e.a) + " " + std::string{binaryOpSpelling(e.bop)} + " " + expr(*e.b) + ")";
            case ExprKind::Logical: return "(" + expr(*e.a) + (e.isAnd ? " && " : " || ") + expr(*e.b) + ")";
            case ExprKind::Assign:
                return "(" + expr(*e.a) + " " + (e.compound ? std::string{binaryOpSpelling(e.bop)} : "") + "= " + expr(*e.b) + ")";
            case ExprKind::IncDec: {
                const char* op = e.increment ? "++" : "--";
                return e.prefix ? "(" + std::string{op} + expr(*e.a) + ")" : "(" + expr(*e.a) + op + ")";
            }
            case ExprKind::Conditional: return "(" + expr(*e.a) + " ? " + expr(*e.b) + " : " + expr(*e.c) + ")";
        }
        return "?";
    }

    static std::string stmt(const Stmt& s, int indent = 0) {
        const std::string pad(std::size_t(indent) * 2, ' ');
        switch (s.kind) {
            case StmtKind::Empty: return pad + ";\n";
            case StmtKind::Expression: return pad + expr(*s.expr) + ";\n";
            case StmtKind::Block: {
                std::string r = pad + "{\n";
                for (const auto& c : s.children) r += stmt(*c, indent + 1);
                return r + pad + "}\n";
            }
            case StmtKind::If:
            case StmtKind::Iff: {
                std::string r = pad + (s.kind == StmtKind::If ? "if (" : "iff (") + expr(*s.expr) + ")\n" + braced(*s.body, indent);
                if (s.elseBody) r += pad + "else\n" + braced(*s.elseBody, indent);
                return r;
            }
            case StmtKind::For:
                return pad + "for (" + (s.init ? expr(*s.init) : "") + "; " + (s.expr ? expr(*s.expr) : "") + "; " +
                       (s.step ? expr(*s.step) : "") + ")\n" + braced(*s.body, indent);
            case StmtKind::While: return pad + "while (" + expr(*s.expr) + ")\n" + braced(*s.body, indent);
            case StmtKind::DoWhile: return pad + "do\n" + braced(*s.body, indent) + pad + "while (" + expr(*s.expr) + ");\n";
            case StmtKind::Break: return pad + "break;\n";
            case StmtKind::Continue: return pad + "continue;\n";
            case StmtKind::Return: return pad + "return" + (s.expr ? "(" + expr(*s.expr) + ")" : "") + ";\n";
        }
        return pad + "?\n";
    }

    static std::string program(const ProgramAst& p) {
        std::string r;
        for (const auto& item : p.order) {
            switch (item.kind) {
                case TopLevelItem::Kind::Extern: {
                    const auto& x = p.externs[item.index];
                    r += "extern " + x.name + "(" + params(x.params) + ") " + std::to_string(x.id) + ";\n";
                    break;
                }
                case TopLevelItem::Kind::Global: {
                    const auto& g = p.globals[item.index];
                    r += std::string{typeName(g.type)} + " ";
                    for (std::size_t i = 0; i < g.vars.size(); ++i) {
                        const auto& v = g.vars[i];
                        if (i) r += ", ";
                        r += v.name;
                        if (v.isArray) r += "[" + std::to_string(v.arraySize) + "]";
                        if (v.init) r += " = " + expr(*v.init);
                        if (!v.arrayInit.empty()) {
                            r += " = {";
                            for (std::size_t k = 0; k < v.arrayInit.size(); ++k) r += (k ? ", " : "") + expr(*v.arrayInit[k]);
                            r += "}";
                        }
                    }
                    r += ";\n";
                    break;
                }
                case TopLevelItem::Kind::Function: {
                    const auto& f = *p.functions[item.index];
                    r += f.name + "(" + params(f.params) + ")\n" + stmt(*f.body, 0);
                    break;
                }
            }
        }
        return r;
    }

private:
    static std::string braced(const Stmt& s, int indent) {
        if (s.kind == StmtKind::Block) return stmt(s, indent);
        return std::string(std::size_t(indent) * 2, ' ') + "{\n" + stmt(s, indent + 1) + std::string(std::size_t(indent) * 2, ' ') + "}\n";
    }
    static std::string params(const std::vector<ParamDecl>& ps) {
        std::string r;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i) r += "; ";
            if (ps[i].type == ParamType::Any) { // untyped extern parameter: `variable`
                r += ps[i].name;
                continue;
            }
            r += std::string{paramTypeName(ps[i].type)};
            if (!ps[i].name.empty()) r += " " + ps[i].name;
        }
        return r;
    }
};

} // namespace gromada::script
