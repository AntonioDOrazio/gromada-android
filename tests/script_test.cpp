// Unit tests of the script module on hand-written synthetic snippets (no game data).
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "script/printer.hpp"
#include "script/recording_host.hpp"
#include "script/script.hpp"
#include "test.hpp"

using namespace gromada::script;

namespace {

// A compiled snippet with a recording host and an interpreter.
struct Harness {
    RecordingHost host;
    Script script;
    std::unique_ptr<Interpreter> vm;

    explicit Harness(std::string_view src, ExecConfig cfg = {}, CompileOptions opt = {}, SourceLoader* includes = nullptr)
        : script{Script::compileText(src, opt, "<test>", includes)} {
        vm = std::make_unique<Interpreter>(script, host, cfg);
    }
    Value tick() { return vm->tick(); }
    Value get(std::string_view name, int i = 0) const { return vm->get(name, i); }
    // First argument of every call to `name`, as ints.
    std::vector<std::int32_t> ints(std::string_view name) const {
        std::vector<std::int32_t> r;
        for (const auto* c : host.callsTo(name)) r.push_back(c->args.empty() ? -999 : c->args[0].toInt());
        return r;
    }
    std::vector<std::string> strings(std::string_view name) const {
        std::vector<std::string> r;
        for (const auto* c : host.callsTo(name)) r.push_back(c->args.empty() ? "?" : c->args[0].toString());
        return r;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> r;
        for (const auto& c : host.calls()) r.push_back(c.name);
        return r;
    }
};

template <class F>
std::optional<ScriptError> errorOf(F&& f) {
    try {
        f();
    } catch (const ScriptError& e) {
        return e;
    }
    return std::nullopt;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

// Externs shared by most snippets.
constexpr std::string_view kExterns = "extern Out(int v) 1; extern OutS(string s) 2; extern Get() 3;\n";

std::string withExterns(std::string_view body) { return std::string{kExterns} + std::string{body}; }

std::vector<Token> lex(std::string_view src) {
    static SourceFiles files;
    return Lexer{src, files.intern("<lex>")}.tokenize();
}

// ---------------------------------------------------------------------------------------------
void testLexer() {
    {
        const auto t = lex("abc_1 42 0x1F 017 'a' '\\n' \"x\\\\y\\\"z\" <<= >>= ++ -- && || != == <= >= -> # ;");
        CHECK(t[0].kind == TokenKind::Identifier && t[0].text == "abc_1");
        CHECK(t[1].kind == TokenKind::Number && t[1].number == 42);
        CHECK_EQ(t[2].number, 31);
        CHECK_EQ(t[3].number, 15); // octal like C
        CHECK(t[4].kind == TokenKind::Char && t[4].number == 'a');
        CHECK_EQ(t[5].number, '\n');
        CHECK(t[6].kind == TokenKind::String);
        CHECK_EQ(t[6].text, std::string{"x\\y\"z"});
        CHECK_EQ(t[6].raw, std::string{"x\\\\y\\\"z"});
        CHECK(t[7].is(Punct::ShlAssign));
        CHECK(t[8].is(Punct::ShrAssign));
        CHECK(t[9].is(Punct::PlusPlus));
        CHECK(t[10].is(Punct::MinusMinus));
        CHECK(t[11].is(Punct::AndAnd));
        CHECK(t[12].is(Punct::OrOr));
        CHECK(t[13].is(Punct::NotEq));
        CHECK(t[14].is(Punct::EqEq));
        CHECK(t[15].is(Punct::LessEq));
        CHECK(t[16].is(Punct::GreaterEq));
        CHECK(t[17].is(Punct::Minus)); // "->" is not an operator of the language
        CHECK(t[18].is(Punct::Greater));
        CHECK(t[19].is(Punct::Hash));
        CHECK(t[20].is(Punct::Semicolon));
        CHECK(t[21].kind == TokenKind::End);
    }
    {
        // CRLF, comments containing non-ASCII bytes, line/column tracking, lineStart flags.
        const auto t = lex("a // \xE0\xE1\xE2 comment\r\n  b /* multi\r\n line \x8F */ c\r\n#x \"\xC0\xFF\"");
        CHECK_EQ(t[0].text, std::string{"a"});
        CHECK(t[0].lineStart);
        CHECK_EQ(t[1].text, std::string{"b"});
        CHECK_EQ(t[1].loc.line, 2u);
        CHECK_EQ(t[1].loc.column, 3u);
        CHECK(t[1].lineStart);
        CHECK_EQ(t[2].text, std::string{"c"});
        CHECK_EQ(t[2].loc.line, 3u);
        CHECK(!t[2].lineStart);
        CHECK(t[3].is(Punct::Hash) && t[3].lineStart && t[3].loc.line == 4u);
        CHECK_EQ(t[5].text, std::string{"\xC0\xFF"}); // literal bytes kept as they are
    }
    {
        // Escapes: hex, octal, unknown escape keeps the character; 0x1A ends the file.
        const auto t = lex("\"\\x41\\101\\q\" '\\0' '\\'' end \x1A garbage \x80");
        CHECK_EQ(t[0].text, std::string{"AAq"});
        CHECK_EQ(t[1].number, 0);
        CHECK_EQ(t[2].number, '\'');
        CHECK_EQ(t[3].text, std::string{"end"});
        CHECK(t[4].kind == TokenKind::End);
    }
    {
        const auto t = lex("0xFFFFFFFF 4294967295 2147483648");
        CHECK_EQ(t[0].number, -1);
        CHECK_EQ(t[1].number, -1);
        CHECK_EQ(t[2].number, std::numeric_limits<std::int32_t>::min());
    }
    auto lexError = [](std::string_view src) { return errorOf([&] { lex(src); }); };
    CHECK(lexError("\"abc").has_value());
    CHECK(lexError("\"ab\ncd\"").has_value());
    CHECK(lexError("/* never closed").has_value());
    CHECK(lexError("a \x80 b").has_value());
    CHECK(lexError("a @ b").has_value());
    CHECK(lexError("0x100000000").has_value());
    CHECK(lexError("09").has_value());
    CHECK(lexError("12ab").has_value());
    CHECK(lexError("''").has_value());
    CHECK(lexError("'ab'").has_value());
    if (const auto e = lexError("ok\n  $")) {
        CHECK(e->phase() == ScriptError::Phase::Lex);
        CHECK_EQ(e->location().line, 2u);
        CHECK_EQ(e->location().column, 3u);
        CHECK(contains(e->what(), "<lex>:2:3"));
    } else {
        CHECK(false);
    }
}

// ---------------------------------------------------------------------------------------------
void testPreprocessor() {
    {
        // Lazy expansion, nesting, redefinition (last wins), #undef, comments after the body.
        Harness h{withExterns("#define B (A+1) // comment\n"
                              "#define A 10\n"
                              "#define TXT \"hi\"\n"
                              "main() { Out(B); OutS(TXT);\n"
                              "#define A 20\n"
                              "Out(B);\n"
                              "#undef A\n"
                              "}\n"
                              "int A = 5;\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{11, 21}));
        CHECK(h.strings("OutS") == (std::vector<std::string>{"hi"}));
        CHECK_EQ(h.get("A"), Value{5}); // after #undef, A is an ordinary identifier
        CHECK_EQ(h.script.warnings().size(), std::size_t{1}); // A redefined with a different value
        CHECK(h.script.macros().contains("B"));
        CHECK(!h.script.macros().contains("A"));
    }
    {
        // Self-referential macro does not loop.
        Harness h{withExterns("int X = 3;\n#define X X+1\nmain() { Out(X); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{4}));
    }
    {
        // #include with DOS separators and any letter case; the name is not escape-processed.
        MemorySourceLoader inc;
        inc.add("lib/Common.INI", withExterns("#define K 7\nint shared = K;\nHelper() { Out(shared); }\n"));
        inc.add("lib/sub/deep.ini", "#include \"lib\\common.ini\"\n");
        Harness h{"#include \"LIB\\SUB\\DEEP.ini\"\nmain() { Helper(); Out(K * 2); }\n", {}, {}, &inc};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{7, 14}));
        CHECK_EQ(h.script.files().size(), std::size_t{3});
    }
    auto ppError = [](std::string_view src, MemorySourceLoader* inc = nullptr) {
        return errorOf([&] { Script::compileText(src, {}, "<test>", inc); });
    };
    {
        MemorySourceLoader inc;
        inc.add("a.ini", "#include \"b.ini\"\n");
        inc.add("b.ini", "#include \"a.ini\"\n");
        const auto e = ppError("#include \"a.ini\"\n", &inc);
        CHECK(e && e->phase() == ScriptError::Phase::Preprocess && contains(e->what(), "recursive"));
    }
    {
        const auto e = ppError("int x;\n#include \"missing.ini\"\n");
        CHECK(e && contains(e->what(), "<test>:2:10") && contains(e->what(), "missing.ini"));
    }
    CHECK(ppError("#define F(x) x\n").has_value());
    CHECK(ppError("#ifdef X\n").has_value());
    CHECK(ppError("#include nothing\n").has_value());
    CHECK(ppError("# 1\n").has_value());
    CHECK(ppError("int a; # define X\n").has_value()); // '#' not at line start is not a directive
    {
        // `#define F (x)` (space) is an object-like macro.
        Harness h{withExterns("#define V (2)\nmain() { Out(V*3); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{6}));
    }
}

// ---------------------------------------------------------------------------------------------
// Differential test: the interpreter must agree with C++ (same precedence and associativity as
// C) on integer expressions. Each case is written once and evaluated by both.
#define EXPR(e) std::pair<std::string_view, std::int32_t>{#e, static_cast<std::int32_t>(e)}
void testExpressionsAgainstCxx() {
    const std::vector<std::pair<std::string_view, std::int32_t>> cases{
        EXPR(1 + 2 * 3),
        EXPR(7 - 3 - 2),
        EXPR(100 / 10 / 5),
        EXPR(10 - 2 * 3 + 4 / 2 % 3),
        EXPR(1 << 2 + 1),
        EXPR(2 + 3 << 1 >> 1),
        EXPR(5 & 3 | 8 ^ 2),
        EXPR(1 | 2 ^ 3 & 4),
        EXPR(6 & 3 == 3),
        EXPR(1 < 2 == 1),
        EXPR(3 > 2 > 1),
        EXPR(2 <= 2 != 0),
        EXPR(!0 + 1),
        EXPR(!1 == 0),
        EXPR(~5 & 0xFF),
        EXPR(~0 >> 28),
        EXPR(-1 >> 1),
        EXPR(-7 / 2),
        EXPR(-7 % 2),
        EXPR(7 % -3),
        EXPR(- -5),
        EXPR(-(-5) * -1),
        EXPR(1 || 0 && 0),
        EXPR(0 && 1 || 1),
        EXPR((1 ? 2 : 3) + 1),
        EXPR(0 ? 1 : 2 ? 3 : 4),
        EXPR(1 ? 0 ? 5 : 6 : 7),
        EXPR('a' + 1),
        EXPR('0' + 123 / 10 % 10),
        EXPR(0x777 & 0x0F0),
        EXPR(1073741823 >> 3),
        EXPR(017 + 1),
        EXPR(3 * (4 + 5) - (6 - 7) * 8),
        EXPR((1 << 14) + 1),
        EXPR(2 > 1 && 3 > 2 || 0),
        EXPR(!(1 & 2) | 4),
    };
    std::string src = withExterns("main() {\n");
    for (const auto& [text, value] : cases) src += "  Out(" + std::string{text} + ");\n";
    src += "}\n";
    Harness h{src};
    h.tick();
    const auto got = h.ints("Out");
    CHECK_EQ(got.size(), cases.size());
    for (std::size_t i = 0; i < cases.size() && i < got.size(); ++i) {
        if (got[i] != cases[i].second) {
            ++gromada_test::failures;
            std::fprintf(stderr, "expression '%.*s': interpreter %d, C++ %d\n", int(cases[i].first.size()),
                         cases[i].first.data(), got[i], cases[i].second);
        }
    }
}
#undef EXPR

// ---------------------------------------------------------------------------------------------
void testParser() {
    auto parseError = [](std::string_view src) { return errorOf([&] { Script::compileText(src); }); };
    {
        // Dangling else binds to the nearest if; `else;` closes an inner if (as the game does).
        Harness h{withExterns("int a, b;\n"
                              "T() {\n"
                              "  if (a)\n"
                              "    if (b) Out(1); else;\n"
                              "  else Out(2);\n"
                              "  if (a) if (b) Out(3); else Out(4);\n"
                              "}\n"
                              "main() { a=0; b=0; T(); a=1; b=0; T(); a=1; b=1; T(); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{2, 4, 1, 3}));
    }
    {
        // Parameters separated by ';' or ','; untyped extern parameter; forward declaration;
        // optional return type keyword; stray ';' at top level.
        Harness h{"extern Out(int v) 1; extern Dump(int file, variable) 2; extern int Count() 3;\n"
                  "Sum(int a; int b, int c);\n"
                  "int Sum(int a; int b, int c) { return(a + b * c); };\n"
                  "main() { Out(Sum(1, 2, 3)); Dump(1, \"x\"); }\n"};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{7}));
        const BuiltinInfo* dump = h.script.builtins().findById(2);
        CHECK(dump && dump->arity() == 2 && dump->params[1].type == ParamType::Any && dump->params[1].name == "variable");
        CHECK(h.script.builtins().findById(3) != nullptr);
    }
    {
        // Arrays: size from the initializer, zero fill, trailing comma, constant size expression.
        Harness h{withExterns("#define N 2\nint a[N*2] = {1, 2,};\nint b[] = {4, 5, 6};\nstring s[3];\n"
                              "main() { Out(a[0]); Out(a[1]); Out(a[3]); Out(b[2]); OutS(s[2]); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{1, 2, 0, 6}));
        CHECK(h.strings("OutS") == (std::vector<std::string>{""}));
        CHECK_EQ(h.script.globals()[*h.script.findGlobal("a")].size, 4);
        CHECK_EQ(h.script.globals()[*h.script.findGlobal("b")].size, 3);
    }
    {
        const auto e = parseError("main() {\n  Out(1)\n}\n");
        CHECK(e && e->phase() == ScriptError::Phase::Parse && contains(e->what(), "<memory>:3:1") && contains(e->what(), "';'"));
    }
    CHECK(parseError("main() { iff (1) ; else ; }").has_value());       // else after iff: unknown meaning
    CHECK(parseError("main() { int local; }").has_value());              // no local declarations
    CHECK(parseError("main() { 1 = 2; }").has_value());
    CHECK(parseError("main() { (1)++; }").has_value());
    CHECK(parseError("int a[0];").has_value());
    CHECK(parseError("int a[2] = {1, 2, 3};").has_value());
    CHECK(parseError("int a[x];").has_value());
    CHECK(parseError("int a[];").has_value());
    CHECK(parseError("main() { else; }").has_value());
    CHECK(parseError("main() { if 1 ; }").has_value());
    CHECK(parseError("main() {").has_value());
    CHECK(parseError("main() { Out(1,); }").has_value());
    CHECK(parseError("x;").has_value());
    CHECK(parseError("extern F() ;").has_value());
    CHECK(parseError("main() { extern F() 1; }").has_value());
    CHECK(parseError("main() { a[1][2]; }").has_value());
    CHECK(parseError("main() { 3(); }").has_value());
    {
        // `!x = F()` is accepted as `!(x = F())` (game compatibility), also through several prefixes.
        Harness h{withExterns("int x, y;\nmain() { if (!x = Get()) Out(1); else Out(2); y = -!x = 0; Out(x); Out(y); }\n")};
        h.host.setReturn("Get", Value{5});
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{2, 0, -1}));
        CHECK(contains(AstPrinter::expr(*h.script.findFunction("main")->body->children[0]->expr), "(!(x = Get()))"));
    }
}

// ---------------------------------------------------------------------------------------------
void testLinker() {
    auto linkError = [](std::string_view src, CompileOptions opt = {}) {
        return errorOf([&] { Script::compileText(src, opt); });
    };
    {
        const auto e = linkError("main() { y = 1; }");
        CHECK(e && e->phase() == ScriptError::Phase::Link && contains(e->what(), "undeclared identifier 'y'"));
    }
    {
        CompileOptions opt;
        opt.implicitGlobals = true;
        const Script s = Script::compileText("main() { y = 1; }", opt);
        CHECK(s.findGlobal("y").has_value());
        CHECK_EQ(s.warnings().size(), std::size_t{1});
    }
    CHECK(linkError("main() { Nope(); }").has_value());
    CHECK(linkError("main() { } main() { }").has_value());
    {
        CompileOptions opt;
        opt.allowFunctionRedefinition = true;
        RecordingHost host;
        Interpreter vm{Script::compileText(withExterns("F() { Out(1); } F() { Out(2); } main() { F(); }"), opt), host};
        vm.tick();
        CHECK_EQ(host.calls().at(0).args.at(0), Value{2});
    }
    CHECK(linkError("main() { break; }").has_value());
    CHECK(linkError("main() { if (1) continue; }").has_value());
    CHECK(linkError("int a; string a;").has_value());
    CHECK(linkError("int a = 1; int a = 2;").has_value());
    CHECK(linkError("string p; F(int p) { }").has_value());          // parameter vs global of another type
    CHECK(linkError("int arr[2]; F(int arr) { }").has_value());
    CHECK(linkError("extern A() 1; extern B() 1;").has_value());      // same id, different name
    CHECK(linkError("extern A() 1; extern A() 2;").has_value());      // same name and arity, two ids
    CHECK(linkError("extern A(int x) 1; main() { A(1, 2, 3); A(); A(1,2); }").has_value() == false);
    CHECK(linkError("extern A() 1; extern A(int x) 2; main() { A(1, 2); }").has_value()); // no overload with 2 params
    CHECK(linkError("F() {} main() { F = 1; }").has_value());          // function used as a variable
    {
        CompileOptions opt;
        opt.strictUserArity = true;
        CHECK(linkError("F(int a) {} main() { F(1, 2); }", opt).has_value());
        opt.requireMain = true;
        CHECK(linkError("F(int a) {}", opt).has_value());
    }
    {
        // Overloads resolved by arity (like StopSFX() / StopSFX(int)); identical redeclaration is fine.
        Harness h{"extern Stop() 10; extern Stop(int n) 11; extern Stop() 10;\nmain() { Stop(); Stop(3); }\n"};
        h.tick();
        CHECK_EQ(h.host.calls().size(), std::size_t{2});
        CHECK_EQ(h.host.calls()[0].id, 10);
        CHECK_EQ(h.host.calls()[1].id, 11);
        CHECK_EQ(h.script.builtins().size(), std::size_t{2});
        CHECK_EQ(h.script.builtins().overloads("Stop").size(), std::size_t{2});
    }
    {
        // Duplicate declaration of the same variable: one storage, warning.
        Harness h{withExterns("int v; int v = 4; main() { Out(v); }")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{4}));
        CHECK_EQ(h.script.warnings().size(), std::size_t{1});
    }
    {
        // Predeclared builtins (no extern needed); the script's own extern wins.
        BuiltinRegistry pre;
        pre.add(BuiltinInfo{77, "Tool", {ParamDecl{ParamType::Int, "x", {}}}, {}});
        pre.add(BuiltinInfo{78, "Out", {ParamDecl{ParamType::Int, "x", {}}}, {}});
        CompileOptions opt;
        opt.predeclared = &pre;
        Script s = Script::compileText(withExterns("main() { Tool(1); Out(2); }"), opt);
        pre = BuiltinRegistry{}; // the script must not depend on the registry after compiling
        RecordingHost host;
        Interpreter vm{s, host};
        vm.tick();
        CHECK_EQ(host.calls().size(), std::size_t{2});
        CHECK_EQ(host.calls()[0].id, 77);
        CHECK_EQ(host.calls()[1].id, 1);
    }
}

// ---------------------------------------------------------------------------------------------
void testIntegersAndTypes() {
    Harness h{withExterns("int i, j; string s, t;\n"
                          "main() {\n"
                          "  i = 2147483647; i = i + 1; Out(i);\n"     // wraps
                          "  i = 5 / 0; Out(i); i = 5 % 0; Out(i);\n"  // defined as 0
                          "  i = -2147483647 - 1; Out(i / -1);\n"
                          "  Out(1 << 33);\n"                          // count masked to 5 bits
                          "  i = 10; i += 5; i -= 3; i *= 2; i /= 4; i %= 4; Out(i);\n"
                          "  i = 6; i &= 3; i |= 8; i ^= 1; i <<= 2; i >>= 1; Out(i);\n"
                          "  i = 5; j = i++ + ++i; Out(j); Out(i); j = i-- - --i; Out(j); Out(i);\n"
                          "  s = 42; OutS(s);\n"                       // int -> string: decimal
                          "  i = \"  -17abc\"; Out(i);\n"              // string -> int: atoi
                          "  i = \"x\"; Out(i);\n"
                          "  s = \"ab\" + 5 + \"cd\"; OutS(s);\n"      // + with a string concatenates
                          "  Out(\"10\" * 2);\n"                       // other operators convert to int
                          "  t = s = 7; OutS(t);\n"
                          "  i = (s = \"9\") + 1; OutS(i);\n"
                          "}\n")};
    h.tick();
    CHECK(h.ints("Out") == (std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min(), 0, 0,
                                                      std::numeric_limits<std::int32_t>::min(), 2, 2, 22, 12, 7, 2, 5,
                                                      -17, 0, 20}));
    CHECK(h.strings("OutS") == (std::vector<std::string>{"42", "ab5cd", "7", "91"}));
}

void testStrings() {
    Harness h{withExterns("string s, e; int n;\n"
                          "main() {\n"
                          "  s = \"dir\\\\file01.dat\";\n"
                          "  s[8] = '0' + 42 / 10; s[9] = '0' + 42 % 10; OutS(s);\n"
                          "  Out(s[0]); Out(s[3]); Out(s[13]); Out(s[14]);\n"     // s[14] is the terminator
                          "  s = \"AB\"; s[2] = 'C'; s[3] = 0; OutS(s);\n"         // append at the end
                          "  s[1] = 0; OutS(s);\n"                                  // NUL truncates
                          "  s[5] = 'Z'; OutS(s);\n"                                // past the end: ignored
                          "  Out(s == \"A\"); Out(s != \"A\"); Out(s == e); Out(e == \"\");\n"
                          "  Out(\"abc\" < \"abd\"); Out(\"b\" > \"abc\"); Out(\"ab\" < \"abc\"); Out(\"\\xE0\" > \"a\");\n"
                          "  Out(\"5\" == 5); Out(\"5x\" == 5);\n"                  // mixed: compared as ints
                          "  if (e) Out(100); if (s) Out(101);\n"                   // truth of strings
                          "  e = \"\\xC0\"; Out(e[0]);\n"
                          "  s = \"x\"; s[0] = \"Q\"; OutS(s);\n"                   // string into a char: first byte
                          "  n = 7; Out(n[0]);\n"                                    // scalar int indexed as array of 1
                          "}\n")};
    h.tick();
    CHECK(h.strings("OutS") == (std::vector<std::string>{"dir\\file42.dat", "ABC", "A", "A", "Q"}));
    CHECK(h.ints("Out") == (std::vector<std::int32_t>{'d', '\\', 't', 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 101, 0xC0, 7}));
    CHECK_EQ(h.host.warnings().size(), std::size_t{1}); // the write past the end
    {
        ExecConfig cfg;
        cfg.signedChars = true;
        Harness hs{withExterns("string e = \"\\xC0\"; main() { Out(e[0]); }"), cfg};
        hs.tick();
        CHECK(hs.ints("Out") == (std::vector<std::int32_t>{-64}));
    }
}

void testArrays() {
    Harness h{withExterns("int a[3] = {1, 2, 3}, k; string names[2] = {\"p\", \"q\"};\n"
                          "main() {\n"
                          "  a = 9; Out(a[0]); Out(a);\n"               // bare array name = element 0
                          "  k = a[1] + a[2]; Out(k);\n"
                          "  a[k - 4] = 8; Out(a[1]);\n"
                          "  Out(a[3]); a[-1] = 5; a[7] = 5;\n"          // out of range: 0 / ignored
                          "  names[1] = names[0] + \"!\"; OutS(names[1]); OutS(names);\n"
                          "  for (k = 0; k < 3; k++) a[k] = a[k] * 10; Out(a[0] + a[1] + a[2]);\n"
                          "}\n")};
    h.tick();
    CHECK(h.ints("Out") == (std::vector<std::int32_t>{9, 9, 5, 8, 0, 200}));
    CHECK(h.strings("OutS") == (std::vector<std::string>{"p!", "p"}));
    CHECK_EQ(h.host.warnings().size(), std::size_t{3});
    {
        ExecConfig cfg;
        cfg.outOfRange = OutOfRangePolicy::Error;
        Harness he{withExterns("int a[2]; main() { a[2] = 1; }"), cfg};
        const auto e = errorOf([&] { he.tick(); });
        CHECK(e && e->phase() == ScriptError::Phase::Runtime && contains(e->what(), "<test>:2:"));
    }
}

void testControlFlow() {
    Harness h{withExterns("int i, j, n;\n"
                          "Twice(int x) { return(x * 2); }\n"
                          "NoValue() { n = n + 1; }\n"
                          "EarlyOut(int x) { if (x > 2) return x; Out(x); return; }\n"
                          "main() {\n"
                          "  for (i = 0; i < 10; i++) { if (i == 2) continue; if (i == 5) break; Out(i); }\n"
                          "  i = 0; while (1) { if (++i >= 3) break; } Out(i);\n"
                          "  i = 0; do { i = i + 10; } while (i < 5); Out(i);\n"
                          "  i = 0; do { if (i == 1) { i = 7; continue; } i++; } while (i < 3); Out(i);\n"
                          "  for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) { if (j == 1) break; n = n + 10; } Out(n);\n"
                          "  Out(Twice(Twice(3)));\n"
                          "  Out(NoValue()); Out(n);\n"
                          "  Out(EarlyOut(5)); Out(EarlyOut(1));\n"
                          "  for (;;) { break; }\n"
                          "  if (Twice(1) == 2) return(77);\n"
                          "  Out(-1);\n"
                          "}\n")};
    const Value r = h.tick();
    CHECK_EQ(r, Value{77});
    CHECK(h.ints("Out") == (std::vector<std::int32_t>{0, 1, 3, 4, 3, 10, 7, 30, 12, 0, 31, 5, 1, 0}));
    CHECK_EQ(h.vm->tickCount(), std::uint64_t{1});
    {
        ExecConfig cfg;
        cfg.defaultReturn = -5;
        Harness hd{withExterns("F() { } main() { Out(F()); }"), cfg};
        CHECK_EQ(hd.tick(), Value{-5});
        CHECK(hd.ints("Out") == (std::vector<std::int32_t>{-5}));
    }
    {
        // Calling a function by name from the host.
        Harness hc{withExterns("int total; Add(int a; int b) { total = total + a + b; return(total); } main() {}")};
        const Value args[] = {Value{2}, Value{3}};
        CHECK_EQ(hc.vm->call("Add", args), Value{5});
        CHECK_EQ(hc.vm->call("Add", args), Value{10});
        CHECK(errorOf([&] { hc.vm->call("Missing"); }).has_value());
    }
    {
        Harness hn{"int x;"};
        CHECK(errorOf([&] { hn.tick(); }).has_value()); // no main
    }
}

void testStaticParameters() {
    // Parameters are global variables with static storage: readable after the call and from
    // other functions; a call with fewer arguments keeps (or resets) the others.
    const std::string src = withExterns("Set(string name; int mode) { }\n"
                                        "Show() { OutS(name); Out(mode); }\n"
                                        "main() { Set(\"first\", 3); Show(); Set(\"second\"); Show(); mode = 9; Show(); }\n");
    {
        Harness h{src};
        h.tick();
        CHECK(h.strings("OutS") == (std::vector<std::string>{"first", "second", "second"}));
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{3, 3, 9}));
        CHECK(h.script.globals()[*h.script.findGlobal("mode")].origin == GlobalVar::Origin::Parameter);
        CHECK(!h.script.warnings().empty()); // Set called with 1 of 2 arguments
    }
    {
        ExecConfig cfg;
        cfg.missingArguments = MissingArguments::Reset;
        Harness h{src, cfg};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{3, 0, 9}));
    }
    {
        // Arguments are evaluated left to right before any parameter is assigned, then converted
        // to the parameter type; extra arguments are evaluated and ignored.
        Harness h{withExterns("int k;\nF(int a; string b) { Out(a); OutS(b); }\n"
                              "main() { k = 1; F(k = k + 1, k * 10, Out(99)); F(\"12\", 5); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{99, 2, 12}));
        CHECK(h.strings("OutS") == (std::vector<std::string>{"20", "5"}));
    }
    {
        // A parameter that has the name of a global shares its storage (warning).
        Harness h{withExterns("int level;\nGo(int level) { }\nmain() { Go(4); Out(level); }\n")};
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{4}));
        CHECK_EQ(h.script.warnings().size(), std::size_t{1});
    }
}

void testIff() {
    // The pattern of the game: main() runs once per tick, each `iff(1){...; return;}` runs once,
    // so consecutive blocks run on consecutive ticks.
    const std::string chain = withExterns("int t;\n"
                                          "main() {\n"
                                          "  iff (1) { Out(1); return; }\n"
                                          "  iff (1) { Out(2); return; }\n"
                                          "  iff (t >= 5) Out(5);\n"
                                          "  Out(10 + t);\n"
                                          "}\n");
    {
        Harness h{chain};
        for (int i = 0; i < 4; ++i) {
            h.vm->set("t", Value{i * 2});
            h.tick();
        }
        // tick1: 1; tick2: 2; tick3 (t=4): 14; tick4 (t=6): 5, 16
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{1, 2, 14, 5, 16}));
        CHECK_EQ(h.script.iffSiteCount(), 3u);
        CHECK(h.vm->iffFired(0) && h.vm->iffFired(1) && h.vm->iffFired(2));
        h.vm->resetIffs();
        h.host.clear();
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{1}));
    }
    {
        // A site inside a loop or a function is still a single site.
        Harness h{withExterns("int i;\nOnce() { iff (1) Out(7); }\n"
                              "main() { for (i = 0; i < 3; i++) iff (i >= 1) Out(i); Once(); Once(); }\n")};
        h.tick();
        h.tick();
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{1, 7}));
    }
    {
        // Condition not evaluated after firing (default) / evaluated with iffEvaluateWhenFired.
        const std::string src = withExterns("main() { iff (Get() >= 0) Out(1); }");
        Harness h{src};
        h.tick();
        h.tick();
        CHECK_EQ(h.host.callsTo("Get").size(), std::size_t{1});
        ExecConfig cfg;
        cfg.iffEvaluateWhenFired = true;
        Harness h2{src, cfg};
        h2.tick();
        h2.tick();
        CHECK_EQ(h2.host.callsTo("Get").size(), std::size_t{2});
        CHECK(h2.ints("Out") == (std::vector<std::int32_t>{1}));
    }
    {
        // Alternative models, selectable while issue #3 confirms the original behaviour.
        const std::string src = withExterns("int c;\nmain() { iff (c) Out(1); }\n");
        const int pattern[] = {1, 1, 0, 1, 0, 0, 1};
        auto run = [&](IffMode mode) {
            ExecConfig cfg;
            cfg.iffMode = mode;
            Harness h{src, cfg};
            for (const int c : pattern) {
                h.vm->set("c", Value{c});
                h.tick();
            }
            return h.host.calls().size();
        };
        CHECK_EQ(run(IffMode::OncePerSite), std::size_t{1});
        CHECK_EQ(run(IffMode::RisingEdge), std::size_t{3});
        CHECK_EQ(run(IffMode::PlainIf), std::size_t{4});
    }
    {
        // `return` inside the body of an iff still marks the site as fired.
        Harness h{withExterns("main() { iff (1) { return(1); } return(2); }")};
        CHECK_EQ(h.tick(), Value{1});
        CHECK_EQ(h.tick(), Value{2});
    }
}

void testBuiltinCalls() {
    Harness h{"extern Move(int unit, int x, int y) 40; extern Name(int id) 41; extern Spawn(int kind; int x) 42;\n"
              "int unit, x = 3; string label;\n"
              "main() {\n"
              "  unit = Spawn(7, x = x * 2);\n"
              "  Move(unit, x, x + 1);\n"
              "  label = Name(unit);\n"
              "  Move(Spawn(1, 2), Spawn(3, 4), 0);\n"
              "}\n"};
    int next = 100;
    h.host.setResponder([&](const BuiltinInfo& b, std::span<const Value> args) -> std::optional<Value> {
        if (b.id == 42) return Value{next++ + argInt(args, 0)};
        if (b.id == 41) return Value{"unit" + argString(args, 0)};
        return std::nullopt;
    });
    h.tick();
    const auto& c = h.host.calls();
    CHECK_EQ(c.size(), std::size_t{6});
    CHECK(c[0].id == 42 && c[0].args == (std::vector<Value>{7, 6}));
    CHECK(c[1].id == 40 && c[1].args == (std::vector<Value>{107, 6, 7}));
    CHECK(c[2].name == "Name" && c[2].result == Value{"unit107"});
    CHECK_EQ(h.get("label"), Value{"unit107"});
    // Nested calls in arguments: evaluated left to right, before the outer call.
    CHECK(c[3].args == (std::vector<Value>{1, 2}) && c[4].args == (std::vector<Value>{3, 4}));
    CHECK(c[5].id == 40 && c[5].args == (std::vector<Value>{102, 105, 0}));
    CHECK_EQ(c[1].str(), std::string{"Move(107, 6, 7)"});
    CHECK_EQ(c[2].str(true), std::string{"Name(107) -> \"unit107\""});
    {
        // Builtin called with a different argument count than declared: warning, args passed as given.
        Harness hw{"extern F(int a, int b) 5; main() { F(1); }"};
        hw.tick();
        CHECK_EQ(hw.host.calls().at(0).args.size(), std::size_t{1});
        CHECK_EQ(hw.script.warnings().size(), std::size_t{1});
    }
    {
        // Tracing user functions.
        ExecConfig cfg;
        cfg.traceUserFunctions = true;
        Harness ht{withExterns("Play(int a; int b) { Out(a + b); } main() { Play(1, 2); }"), cfg};
        ht.tick();
        CHECK(ht.names() == (std::vector<std::string>{"main", "Play", "Out"}));
        CHECK(ht.host.calls()[1].userFunction && ht.host.calls()[1].args == (std::vector<Value>{1, 2}));
        CHECK(!ht.host.calls()[2].userFunction);
    }
    {
        // Initializers run in the constructor and may call builtins.
        Harness hi{withExterns("int start = Get() + 1; main() { Out(start); }")};
        CHECK_EQ(hi.host.calls().size(), std::size_t{1});
        hi.tick();
        CHECK(hi.ints("Out") == (std::vector<std::int32_t>{1}));
    }
}

void testShortCircuit() {
    const std::string src = withExterns("main() { if (0 && Get()) Out(1); if (1 || Get()) Out(2); Out(Get() && 0); }");
    {
        Harness h{src};
        h.tick();
        CHECK_EQ(h.host.callsTo("Get").size(), std::size_t{1});
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{2, 0}));
    }
    {
        ExecConfig cfg;
        cfg.shortCircuit = false;
        Harness h{src, cfg};
        h.tick();
        CHECK_EQ(h.host.callsTo("Get").size(), std::size_t{3});
        CHECK(h.ints("Out") == (std::vector<std::int32_t>{2, 0}));
    }
}

void testLimits() {
    {
        ExecConfig cfg;
        cfg.maxSteps = 1000;
        Harness h{withExterns("int i; main() { iff (1) return; while (1) i++; }"), cfg};
        h.tick();
        const auto e = errorOf([&] { h.tick(); });
        CHECK(e && e->phase() == ScriptError::Phase::Runtime && contains(e->what(), "step limit"));
        // The budget is per entry: a later tick can run again.
        h.vm->config().maxSteps = 5000;
        CHECK(errorOf([&] { h.tick(); }).has_value());
    }
    {
        Harness h{withExterns("int n; R() { n++; R(); } main() { R(); }")};
        const auto e = errorOf([&] { h.tick(); });
        CHECK(e && contains(e->what(), "call depth"));
        CHECK_EQ(h.get("n"), Value{std::int32_t(h.vm->config().maxCallDepth - 1)});
    }
}

void testGlobalCarryOver() {
    // A "menu" script sets globals, the next script inherits them.
    const std::string common = "int level, flags = 1, counter; string next, name = \"x\"; int list[3];\n";
    Harness first{withExterns(common + "main() { level = 5; flags = 7; counter = 3; next = \"go\"; name = \"y\"; list[2] = 4; }")};
    first.tick();
    const GlobalSnapshot snap = first.vm->snapshot();
    CHECK_EQ(snap.vars.at("level").ints.at(0), 5);
    {
        Harness second{withExterns(common + "int extra; main() { }")};
        second.vm->inheritGlobals(snap); // defaults: ints yes, strings no, initialized vars keep their value
        CHECK_EQ(second.get("level"), Value{5});
        CHECK_EQ(second.get("counter"), Value{3});
        CHECK_EQ(second.get("list", 2), Value{4});
        CHECK_EQ(second.get("flags"), Value{1});
        CHECK_EQ(second.get("next"), Value{""});
        CHECK_EQ(second.get("name"), Value{"x"});
    }
    {
        Harness second{withExterns("string level; int list[2]; int flags = 0; string next; main() { }")};
        CarryOverPolicy p;
        p.strings = true;
        p.overwriteInitialized = true;
        second.vm->inheritGlobals(snap, p);
        CHECK_EQ(second.get("level"), Value{""}); // type differs: not copied
        CHECK_EQ(second.get("flags"), Value{7});
        CHECK_EQ(second.get("next"), Value{"go"});
        CHECK_EQ(second.get("list", 1), Value{0});
    }
    // Host-side access errors are std::out_of_range (not script errors).
    CHECK(!first.vm->hasGlobal("nothing"));
    auto throwsOutOfRange = [](auto&& f) {
        try {
            f();
        } catch (const std::out_of_range&) {
            return true;
        }
        return false;
    };
    CHECK(throwsOutOfRange([&] { (void)first.vm->get("nothing"); }));
    CHECK(throwsOutOfRange([&] { (void)first.vm->get("list", 3); }));
    CHECK(throwsOutOfRange([&] { first.vm->set("list", Value{1}, -1); }));
}

void testPrinterRoundTrip() {
    const std::string src = withExterns("#define M 3\n"
                                        "int a[M] = {1, -2, 0xFFFFFFFF}, b; string s = \"q\\\\\\\"\\x01\";\n"
                                        "F(int p; string q) { if (p) if (q) return(1); else; else return(-p); }\n"
                                        "main() {\n"
                                        "  for (b = 0; b < M; b++) { a[b] += b << 1 | 1; }\n"
                                        "  while (b--) ; do b++; while (b < 2);\n"
                                        "  iff (a[0] && !a[1] || ~b) Out(b ? F(1, \"x\") : -F(0, s));\n"
                                        "  s[0] = 'z'; b = !b = 1;\n"
                                        "}\n");
    const Script s1 = Script::compileText(src);
    const std::string p1 = AstPrinter::program(s1.ast());
    const Script s2 = Script::compileText(p1);
    const std::string p2 = AstPrinter::program(s2.ast());
    CHECK_EQ(p1, p2);
    CHECK(contains(p1, "(a[b] += ((b << 1) | 1))"));
    CHECK(contains(p1, "extern F") == false);
    // Both programs behave the same.
    RecordingHost h1, h2;
    Interpreter v1{s1, h1}, v2{s2, h2};
    v1.tick();
    v2.tick();
    CHECK_EQ(h1.calls().size(), h2.calls().size());
    for (std::size_t i = 0; i < h1.calls().size() && i < h2.calls().size(); ++i) CHECK_EQ(h1.calls()[i].str(), h2.calls()[i].str());
    CHECK_EQ(v1.get("b"), v2.get("b"));
}

void testKnownBuiltins() {
    BuiltinRegistry r;
    {
        // Copies are independent of the original (indices point into each copy's storage).
        const BuiltinRegistry original = knownBuiltinRegistry();
        BuiltinRegistry copy{original};
        r = copy;
    }
    CHECK_EQ(r.size(), kKnownBuiltins.size());
    const BuiltinInfo* load = r.findById(toInt(BuiltinId::Load));
    CHECK(load && load->name == "Load" && load->arity() == 1 && load->params[0].type == ParamType::String);
    CHECK_EQ(r.resolve("StopSFX", 0)->id, toInt(BuiltinId::StopSFX));
    CHECK_EQ(r.resolve("StopSFX", 1)->id, toInt(BuiltinId::StopSFXOne));
    CHECK_EQ(r.resolve("SetVid", 3)->id, toInt(BuiltinId::SetVid));
    CHECK(r.resolve("NoSuchBuiltin", 0) == nullptr);
    // IDs are unique and the table is sorted.
    for (std::size_t i = 1; i < kKnownBuiltins.size(); ++i) CHECK(toInt(kKnownBuiltins[i - 1].id) < toInt(kKnownBuiltins[i].id));
    // A script can call them without extern declarations when compiled with the table.
    CompileOptions opt;
    opt.predeclared = &r;
    Harness h{"main() { SetPosition(10, 20); StopSFX(); StopSFX(2); }", {}, opt};
    h.tick();
    CHECK(h.host.calls().size() == 3 && h.host.calls()[0].id == toInt(BuiltinId::SetPosition));
    CHECK(h.host.calls()[2].id == toInt(BuiltinId::StopSFXOne));
}

void testSimulatedWorld() {
    BuiltinRegistry r = knownBuiltinRegistry();
    CompileOptions opt;
    opt.predeclared = &r;
    Harness h{"int u, v, w; main() { u = Create(1, 2, 3, 4); SetFlagman(0, u); v = Flagman(0); w = Random(3); "
              "Print(GetTime()); Print(strlen(\"abcd\")); }",
              {}, opt};
    SimulatedWorld world;
    world.timeMs = 1234;
    h.host.setResponder([&](const BuiltinInfo& b, std::span<const Value> a) { return world(b, a); });
    h.tick();
    CHECK_EQ(h.get("u"), Value{1000});
    CHECK_EQ(h.get("v"), Value{1000});
    CHECK(h.get("w").toInt() >= 0 && h.get("w").toInt() < 3);
    CHECK(h.ints("Print") == (std::vector<std::int32_t>{1234, 4}));
}

} // namespace

int main() {
    testLexer();
    testPreprocessor();
    testExpressionsAgainstCxx();
    testParser();
    testLinker();
    testIntegersAndTypes();
    testStrings();
    testArrays();
    testControlFlow();
    testStaticParameters();
    testIff();
    testBuiltinCalls();
    testShortCircuit();
    testLimits();
    testGlobalCarryOver();
    testPrinterRoundTrip();
    testKnownBuiltins();
    testSimulatedWorld();
    return gromada_test::finish();
}
