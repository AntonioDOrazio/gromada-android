#pragma once
// Compilation of a script: preprocess, parse, link.
//
// Linking resolves every name once, so execution never looks names up:
//  - global variables get a slot index;
//  - function parameters are global variables too (static storage, visible from every function):
//    the game reads `nextmapeffect`, a parameter of NextMap(), from Tact() and from the menu
//    scripts, so a parameter must outlive the call and be visible outside its function;
//  - calls are bound to a user function (precedence) or to a builtin declared with `extern`
//    (falling back to CompileOptions::predeclared);
//  - every `iff` statement gets the index of its once-flag.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "script/ast.hpp"
#include "script/builtins.hpp"
#include "script/parser.hpp"
#include "script/preprocessor.hpp"
#include "script/source.hpp"

namespace gromada::script {

struct CompileOptions {
    // Builtins available even without an `extern` declaration (e.g. knownBuiltinRegistry()).
    // The script's own extern declarations take precedence.
    const BuiltinRegistry* predeclared = nullptr;
    // Undeclared identifiers become int globals (with a warning) instead of a link error.
    bool implicitGlobals = false;
    // A second definition of a function replaces the first (with a warning) instead of an error.
    bool allowFunctionRedefinition = false;
    // Calling a user function with more arguments than parameters is an error instead of a
    // warning (extra arguments are evaluated and ignored).
    bool strictUserArity = false;
    // Require a main() function.
    bool requireMain = false;
};

struct GlobalVar {
    enum class Origin : std::uint8_t { Declared, Parameter, Implicit };
    std::string name;
    ValueType type = ValueType::Int;
    bool isArray = false;
    std::int32_t size = 1;
    SourceLocation loc;
    Origin origin = Origin::Declared;
    const VarDeclarator* decl = nullptr; // declaration carrying the initializer, if any
    bool hasInitializer() const { return decl && decl->hasInit(); }
};

class Script {
public:
    // Compiles `mainPath` (loaded through `loader`, which also serves #include).
    static Script compile(SourceLoader& loader, std::string_view mainPath, const CompileOptions& options = {}) {
        auto impl = std::make_shared<Impl>();
        impl->options = options;
        Preprocessor pp{loader, impl->files, &impl->warnings};
        const auto tokens = pp.run(mainPath);
        impl->macros = pp.macros();
        impl->fileList = pp.files();
        impl->ast = Parser{tokens}.parseProgram();
        Linker{*impl}.run();
        impl->options.predeclared = nullptr; // not needed (nor guaranteed alive) after linking
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

    const std::vector<GlobalVar>& globals() const { return impl_->globals; }
    std::optional<std::int32_t> findGlobal(std::string_view name) const {
        const auto it = impl_->globalIndex.find(std::string{name});
        if (it == impl_->globalIndex.end()) return std::nullopt;
        return it->second;
    }
    const FunctionDef* findFunction(std::string_view name) const {
        const auto it = impl_->functionIndex.find(std::string{name});
        return it == impl_->functionIndex.end() ? nullptr : it->second;
    }
    const std::vector<std::unique_ptr<FunctionDef>>& functions() const { return impl_->ast.functions; }
    // Builtins declared by the script's extern declarations.
    const BuiltinRegistry& builtins() const { return impl_->builtins; }
    const std::map<std::string, MacroDefinition>& macros() const { return impl_->macros; }
    const ProgramAst& ast() const { return impl_->ast; }
    std::uint32_t iffSiteCount() const { return impl_->iffSites; }
    // Source location of every iff site, by index.
    const std::vector<SourceLocation>& iffSites() const { return impl_->iffLocations; }
    const std::vector<std::string>& warnings() const { return impl_->warnings; }
    const std::vector<std::string>& files() const { return impl_->fileList; }

private:
    struct Impl {
        CompileOptions options;
        SourceFiles files;
        std::vector<std::string> fileList;
        std::vector<std::string> warnings;
        std::map<std::string, MacroDefinition> macros;
        ProgramAst ast;
        BuiltinRegistry builtins;
        BuiltinRegistry predeclaredUsed; // builtins taken from CompileOptions::predeclared
        std::vector<GlobalVar> globals;
        std::map<std::string, std::int32_t> globalIndex;
        std::map<std::string, const FunctionDef*> functionIndex;
        std::uint32_t iffSites = 0;
        std::vector<SourceLocation> iffLocations;
    };

    class Linker {
    public:
        explicit Linker(Impl& impl) : m_{impl} {}

        void run() {
            for (const auto& ext : m_.ast.externs) {
                BuiltinInfo info;
                info.id = ext.id;
                info.name = ext.name;
                info.params = ext.params;
                info.loc = ext.loc;
                m_.builtins.add(std::move(info));
            }
            // Declarations first (in source order), so that uses may precede declarations.
            for (const auto& item : m_.ast.order) {
                if (item.kind == TopLevelItem::Kind::Global) declareGlobals(m_.ast.globals[item.index]);
                else if (item.kind == TopLevelItem::Kind::Function) declareFunction(*m_.ast.functions[item.index]);
            }
            for (auto& g : m_.ast.globals)
                for (auto& v : g.vars) {
                    if (v.init) linkExpr(*v.init);
                    for (auto& e : v.arrayInit) linkExpr(*e);
                }
            for (auto& fn : m_.ast.functions) {
                loopDepth_ = 0;
                linkStmt(*fn->body);
            }
            if (m_.options.requireMain && !m_.functionIndex.contains("main"))
                throw ScriptError(ScriptError::Phase::Link, {}, "the script has no main() function");
        }

    private:
        [[noreturn]] static void fail(const SourceLocation& loc, std::string msg) {
            throw ScriptError(ScriptError::Phase::Link, loc, std::move(msg));
        }
        void warn(const SourceLocation& loc, const std::string& msg) { m_.warnings.push_back(loc.str() + ": warning: " + msg); }

        std::int32_t addGlobal(GlobalVar v) {
            const auto slot = static_cast<std::int32_t>(m_.globals.size());
            m_.globalIndex[v.name] = slot;
            m_.globals.push_back(std::move(v));
            return slot;
        }

        void declareGlobals(const GlobalDecl& decl) {
            for (const auto& d : decl.vars) {
                GlobalVar v;
                v.name = d.name;
                v.type = decl.type;
                v.isArray = d.isArray;
                v.size = d.arraySize;
                v.loc = d.loc;
                v.decl = &d;
                if (const auto it = m_.globalIndex.find(d.name); it != m_.globalIndex.end()) {
                    GlobalVar& old = m_.globals[it->second];
                    if (old.type != v.type || old.isArray != v.isArray || old.size != v.size)
                        fail(d.loc, std::format("'{}' redeclared with a different type (previous declaration at {})", d.name,
                                                old.loc.str()));
                    if (old.hasInitializer() && v.hasInitializer())
                        fail(d.loc, std::format("'{}' initialized twice (previous declaration at {})", d.name, old.loc.str()));
                    warn(d.loc, std::format("'{}' redeclared (previous declaration at {}); both names share one variable",
                                            d.name, old.loc.str()));
                    if (v.hasInitializer()) old.decl = &d;
                    continue;
                }
                addGlobal(std::move(v));
            }
        }

        void declareFunction(FunctionDef& fn) {
            if (Parser::isKeyword(fn.name)) fail(fn.loc, "'" + fn.name + "' is a keyword");
            if (const auto it = m_.functionIndex.find(fn.name); it != m_.functionIndex.end()) {
                if (!m_.options.allowFunctionRedefinition)
                    fail(fn.loc, std::format("function '{}' already defined at {}", fn.name, it->second->loc.str()));
                warn(fn.loc, std::format("function '{}' redefined (previous definition at {})", fn.name, it->second->loc.str()));
            }
            m_.functionIndex[fn.name] = &fn;
            fn.paramSlots.clear();
            for (const auto& p : fn.params) {
                const ValueType type = p.type == ParamType::String ? ValueType::String : ValueType::Int;
                if (const auto it = m_.globalIndex.find(p.name); it != m_.globalIndex.end()) {
                    const GlobalVar& old = m_.globals[it->second];
                    if (old.type != type || old.isArray)
                        fail(p.loc, std::format("parameter '{}' of '{}' conflicts with '{}' declared at {}", p.name, fn.name,
                                                p.name, old.loc.str()));
                    warn(p.loc, std::format("parameter '{}' of '{}' shares its storage with the global declared at {}",
                                            p.name, fn.name, old.loc.str()));
                    fn.paramSlots.push_back(it->second);
                    continue;
                }
                GlobalVar v;
                v.name = p.name;
                v.type = type;
                v.loc = p.loc;
                v.origin = GlobalVar::Origin::Parameter;
                fn.paramSlots.push_back(addGlobal(std::move(v)));
            }
        }

        std::int32_t resolveVariable(const std::string& name, const SourceLocation& loc) {
            if (const auto it = m_.globalIndex.find(name); it != m_.globalIndex.end()) return it->second;
            if (m_.functionIndex.contains(name) || !m_.builtins.overloads(name).empty())
                fail(loc, "function '" + name + "' used as a variable");
            if (!m_.options.implicitGlobals) fail(loc, "undeclared identifier '" + name + "'");
            warn(loc, "undeclared identifier '" + name + "' implicitly declared as a global int");
            GlobalVar v;
            v.name = name;
            v.loc = loc;
            v.origin = GlobalVar::Origin::Implicit;
            return addGlobal(std::move(v));
        }

        void linkExpr(Expr& e) {
            switch (e.kind) {
                case ExprKind::IntLiteral:
                case ExprKind::StrLiteral: return;
                case ExprKind::Variable: e.slot = resolveVariable(e.text, e.loc); return;
                case ExprKind::Index:
                    e.slot = resolveVariable(e.text, e.loc);
                    linkExpr(*e.a);
                    return;
                case ExprKind::Call: {
                    for (auto& a : e.args) linkExpr(*a);
                    const std::size_t argc = e.args.size();
                    if (const auto it = m_.functionIndex.find(e.text); it != m_.functionIndex.end()) {
                        e.function = it->second;
                        const std::size_t n = e.function->params.size();
                        if (argc > n) {
                            if (m_.options.strictUserArity)
                                fail(e.loc, std::format("'{}' takes {} argument(s), {} given", e.text, n, argc));
                            warn(e.loc, std::format("'{}' takes {} argument(s), {} given: extra arguments are ignored", e.text, n, argc));
                        } else if (argc < n) {
                            warn(e.loc, std::format("'{}' takes {} argument(s), {} given: see ExecConfig::missingArguments", e.text, n, argc));
                        }
                        return;
                    }
                    const BuiltinInfo* b = m_.builtins.resolve(e.text, argc);
                    if (!b && m_.builtins.overloads(e.text).empty() && m_.options.predeclared) {
                        // Copy into the script so it does not depend on the predeclared registry's lifetime.
                        if (const BuiltinInfo* pre = m_.options.predeclared->resolve(e.text, argc))
                            b = &m_.predeclaredUsed.add(*pre);
                    }
                    if (!b) {
                        if (!m_.builtins.overloads(e.text).empty())
                            fail(e.loc, std::format("no overload of builtin '{}' takes {} argument(s)", e.text, argc));
                        fail(e.loc, "call to undeclared function '" + e.text + "'");
                    }
                    if (b->arity() != argc)
                        warn(e.loc, std::format("builtin '{}' declared with {} parameter(s), called with {}", b->signature(),
                                                b->arity(), argc));
                    e.builtin = b;
                    return;
                }
                case ExprKind::Unary:
                case ExprKind::IncDec: linkExpr(*e.a); return;
                case ExprKind::Binary:
                case ExprKind::Logical:
                case ExprKind::Assign:
                    linkExpr(*e.a);
                    linkExpr(*e.b);
                    return;
                case ExprKind::Conditional:
                    linkExpr(*e.a);
                    linkExpr(*e.b);
                    linkExpr(*e.c);
                    return;
            }
        }

        void linkStmt(Stmt& s) {
            switch (s.kind) {
                case StmtKind::Empty: return;
                case StmtKind::Expression: linkExpr(*s.expr); return;
                case StmtKind::Block:
                    for (auto& c : s.children) linkStmt(*c);
                    return;
                case StmtKind::If:
                    linkExpr(*s.expr);
                    linkStmt(*s.body);
                    if (s.elseBody) linkStmt(*s.elseBody);
                    return;
                case StmtKind::Iff:
                    s.iffSite = m_.iffSites++;
                    m_.iffLocations.push_back(s.loc);
                    linkExpr(*s.expr);
                    linkStmt(*s.body);
                    return;
                case StmtKind::For:
                    if (s.init) linkExpr(*s.init);
                    if (s.expr) linkExpr(*s.expr);
                    if (s.step) linkExpr(*s.step);
                    ++loopDepth_;
                    linkStmt(*s.body);
                    --loopDepth_;
                    return;
                case StmtKind::While:
                case StmtKind::DoWhile:
                    linkExpr(*s.expr);
                    ++loopDepth_;
                    linkStmt(*s.body);
                    --loopDepth_;
                    return;
                case StmtKind::Break:
                case StmtKind::Continue:
                    if (loopDepth_ == 0) fail(s.loc, std::format("'{}' outside of a loop", s.kind == StmtKind::Break ? "break" : "continue"));
                    return;
                case StmtKind::Return:
                    if (s.expr) linkExpr(*s.expr);
                    return;
            }
        }

        Impl& m_;
        int loopDepth_ = 0;
    };

    explicit Script(std::shared_ptr<Impl> impl) : impl_{std::move(impl)} {}

    std::shared_ptr<Impl> impl_;
};

} // namespace gromada::script
