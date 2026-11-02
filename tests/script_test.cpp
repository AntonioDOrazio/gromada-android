// Unit tests of the script module on hand-written synthetic snippets (no game data).
// Every rule of docs/re/runtime.md §1-2 implemented by src/script has a test here.
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "script/recording_host.hpp"
#include "script/script.hpp"
#include "test.hpp"

using namespace gromada::script;

namespace {

// Builtins used by the snippets (real IDs of the game: what an opcode does is fixed by the ID).
constexpr std::string_view kExterns =
    "extern Print(int i) 66;\n"          // pushes nothing
    "extern Message(string text) 100;\n" // pushes nothing
    "extern GetKey() 88;\n"              // pushes an int (the tests' host: 1, 2, 3...)
    "extern strlen(string str) 103;\n"   // pushes an int
    "extern GetVidName(int nvid) 124;\n" // pushes a string
    "extern FRead(int file) 112;\n"      // pushes a string (one static buffer)
    "extern GetNetInfo(int type) 129;\n" // pushes for types 1..6 and 10..25 only
    "extern PlaySFX(int nsfx) 91;\n";    // pushes nothing

std::string withExterns(std::string_view body) { return std::string{kExterns} + std::string{body}; }

// A compiled snippet with a recording host and an interpreter.
struct Harness {
    RecordingHost host;
    Script script;
    std::unique_ptr<Interpreter> vm;
    std::int32_t counter = 0;

    explicit Harness(std::string_view src, ExecConfig cfg = {}, CompileOptions opt = {})
        : script{Script::compileText(withExterns(src), opt, "<test>")} {
        host.setResponder([this](const BuiltinInfo& b, std::span<const Value> args) -> std::optional<Value> {
            switch (b.id) {
                case 88: return Value{++counter};
                case 103: return Value{std::int32_t(argString(args, 0).size())};
                case 124: return Value{"vid" + std::to_string(argInt(args, 0))};
                case 112: return Value{"line" + std::to_string(++counter)};
                case 129: return Value{argInt(args, 0) * 100};
                default: return std::nullopt;
            }
        });
        vm = std::make_unique<Interpreter>(script, host, cfg);
    }
    Value tick() { return vm->tick(); }
    Value get(std::string_view name, int i = 0) const { return vm->get(name, i); }
    std::int32_t geti(std::string_view name, int i = 0) const { return vm->get(name, i).toInt(); }
    std::string gets(std::string_view name, int i = 0) const { return vm->get(name, i).toString(); }
    // First argument of every call to `name`.
    std::vector<Value> args(std::string_view name) const {
        std::vector<Value> r;
        for (const auto* c : host.callsTo(name)) r.push_back(c->args.empty() ? Value{-999} : c->args[0]);
        return r;
    }
    std::vector<std::int32_t> printed() const {
        std::vector<std::int32_t> r;
        for (const auto& v : args("Print")) r.push_back(v.toInt());
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

std::optional<ScriptError> compileError(std::string_view src) {
    return errorOf([&] { Script::compileText(withExterns(src), {}, "<test>"); });
}

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------------------------
// Compiler: emitted bytecode for representative constructs (docs/re/runtime.md §2.3).
void testBytecode() {
    {
        // a[i] = e: the value first, then the index; top-level declarations leave a dead EOS.
        const Script s = Script::compileText("int a, v[2]; main(){v[a]=5;}");
        const Bytes expect{0x1E, 9, 0, 0, 0, 0x1C, 0x30, 0x75, // CALL main; JMP +30000
                           0x19,                               // `int a, v[2];` -> EOS (dead)
                           0x01, 5, 0, 0, 0, 0x17, 0, 0x24, 0x18, 1, 0x19, // PUSHI 5; PUSHV a; INDEX; STORE v; EOS
                           0x1F, 0x19};                        // RET; dead EOS ('}' patched to ';')
        CHECK(s.code() == expect);
        CHECK_EQ(s.mainEntry(), 9u);
    }
    {
        // User call: every argument is stored into its parameter cell, then CALL.
        const Script s = Script::compileText("F(int p){} main(){F(7);}");
        const Bytes expect{0x1E, 10, 0, 0, 0, 0x1C, 0x30, 0x75, 0x1F, 0x19, // F: RET; dead EOS
                           0x01, 7, 0, 0, 0, 0x18, 0, 0x19, 0x1E, 8, 0, 0, 0, 0x19, 0x1F, 0x19};
        CHECK(s.code() == expect);
        CHECK_EQ(s.findFunction("F")->entry, 8u);
        CHECK_EQ(s.findFunction("F")->paramBase, 0);
        CHECK_EQ(s.findFunction("F")->paramCount, 1);
    }
    {
        // Builtins: arguments pushed left to right, then the opcode = the extern's ID.
        const Script s = Script::compileText("extern Print(int i) 66; main(){Print(1+2);}");
        const Bytes body{0x01, 1, 0, 0, 0, 0x01, 2, 0, 0, 0, 0x03, 66, 0x19, 0x1F, 0x19};
        CHECK(Bytes(s.code().begin() + 9, s.code().end()) == body); // after prologue + extern's EOS
    }
    {
        // if/else, while, for, break, iff, return(e): jump layout.
        const Script s = Script::compileText(
            "int i; main(){ if(i) i=1; else i=2; while(i) break; for(i=0;i<2;i++) ; iff(1) return(3); }");
        const std::string d = s.disassemble();
        CHECK(contains(d, "JZ"));
        CHECK(contains(d, "IFF"));
        CHECK(contains(d, "RET"));
        CHECK_EQ(s.iffSiteCount(), 1u);
        CHECK(s.iffSites()[0].opcodeOffset > s.iffSites()[0].conditionStart);
    }
}

// Lexical rules, literals, #define, #include.
void testLexical() {
    {
        Harness h{"int a, b, c, d, e, f, g;\n"
                  "main(){ /* comment */ a=0x1F; b=017; c=09; // octal like sscanf %i; '9' is skipped\n"
                  "  d='a'; e='\xE9'; f=-5; g=10/3; }"};
        h.tick();
        CHECK_EQ(h.geti("a"), 31);
        CHECK_EQ(h.geti("b"), 15);
        CHECK_EQ(h.geti("c"), 0);
        CHECK_EQ(h.geti("d"), 97);
        CHECK_EQ(h.geti("e"), -23); // char is signed
        CHECK_EQ(h.geti("f"), -5);
        CHECK_EQ(h.geti("g"), 3);
    }
    {
        // String escapes: \n \r, any other escaped char is itself; \" ends the string.
        Harness h{"string s, t; main(){ s=\"a\\nb\\rc\\\\d\\te\"; t=\"x\\\"; }"};
        h.tick();
        CHECK_EQ(h.gets("s"), std::string("a\nb\rc\\dte"));
        CHECK_EQ(h.gets("t"), std::string("x"));
    }
    // Identifiers: case sensitive, at most 31 characters, must start with a letter.
    CHECK(!compileError("int Abc, abc; main(){Abc=1;abc=2;}"));
    CHECK(!compileError("int a234567890123456789012345678901; main(){}"));
    CHECK(compileError("int a2345678901234567890123456789012; main(){}").has_value());
    CHECK(compileError("int _x; main(){ _x=1; }").has_value()); // declared, but not usable
    // A character literal is one byte between quotes: no escapes.
    CHECK(compileError("int c; main(){c='\\n';}").has_value());
    {
        // #define: int (sscanf %i) or "string" (no escapes); the first definition wins.
        Harness h{"#define N 0x10\n#define N 99\n#define S \"a\\b\"\nint n; string s; main(){n=N; s=S;}"};
        h.tick();
        CHECK_EQ(h.geti("n"), 16);
        CHECK_EQ(h.gets("s"), std::string("a\\b"));
        CHECK_EQ(h.script.defines().size(), std::size_t{3});
        CHECK(std::ranges::any_of(h.script.notes(), [](const CompileNote& n) { return n.kind == "duplicate-define"; }));
    }
    CHECK(compileError("#undef X\nmain(){}").has_value());
    CHECK(compileError("#if 1\nmain(){}").has_value());
    {
        // #include: path relative to the loader's root, taken verbatim (single backslash).
        MemorySourceLoader files;
        files.add("maps/inc.ini", "#define K 7\nint shared;\n");
        files.add("maps/main.ini", "#include \"maps\\inc.ini\"\nmain(){shared=K;}");
        const Script s = Script::compile(files, "maps/main.ini");
        RecordingHost host;
        Interpreter vm{s, host};
        vm.tick();
        CHECK_EQ(vm.get("shared").toInt(), 7);
        CHECK_EQ(s.files().size(), std::size_t{2});
    }
    // Errors carry the original's message and the position.
    if (auto e = compileError("main(){ x=1; }")) {
        CHECK(contains(e->what(), "Unknown name x"));
        CHECK(contains(e->what(), "<test>:"));
    } else {
        CHECK(false);
    }
}

// Declarations, names, statements the original does not have.
void testDeclarationsAndStatements() {
    CHECK(compileError("main(){x=1;} int x;").has_value());              // declared after use
    CHECK(compileError("int x; int x; main(){}").has_value());           // redefinition
    CHECK(compileError("int p; F(int p){} main(){}").has_value());       // a parameter is a global
    CHECK(compileError("string s=\"x\"; main(){}").has_value());         // strings have no initializer
    CHECK(compileError("int x=Q; main(){}").has_value());                // initializers are literals
    CHECK(compileError("main(){ }").has_value());                        // `{ }` with a blank inside
    CHECK(!compileError("main(){}").has_value());
    CHECK(compileError("int v[2]; main(){ v[0] = 1; }").has_value());   // no blank skipped after ']'
    CHECK(!compileError("int v[2]; main(){ v[0]=1; v [1]= 2; }").has_value());
    CHECK(compileError("int x; main(){ return x; }").has_value());       // return needs parentheses
    CHECK(!compileError("int x; main(){ return(x); }").has_value());
    CHECK(compileError("int x; main(){ x = 1 ? 2 : 3; }").has_value());  // no ?:
    CHECK(compileError("main(){ do ; while(1); }").has_value());         // no do
    CHECK(compileError("int i; main(){ while(i) continue; }").has_value()); // no continue
    CHECK(compileError("main(){ break; }").has_value());                 // break outside a loop
    CHECK(compileError("int x; main(){ x=!!x; }").has_value());          // one unary operator only
    CHECK(compileError("int x; main(){ x += 1; }").has_value());         // no compound assignment
    if (auto e = errorOf([] { Script::compileText("int x;"); })) CHECK(contains(e->what(), "Not enough main()"));
    else CHECK(false);
    {
        CompileOptions opt;
        opt.requireMain = false;
        CHECK(!errorOf([&] { Script::compileText("int x;", opt); }).has_value());
    }
    {
        // Every variable is a global cell: globals, parameters, declarations inside functions,
        // in compile order; an array is consecutive cells with the same name.
        const Script s = Script::compileText(
            "int a=3, v[3]={1,2}; string s, t[2];\n"
            "F(int p; string q) { int local=-4; }\n"
            "main(){}");
        const auto& c = s.cells();
        CHECK_EQ(c.size(), std::size_t{10});
        const std::vector<std::string> names{"a", "v", "v", "v", "s", "t", "t", "p", "q", "local"};
        for (std::size_t i = 0; i < names.size(); ++i) CHECK_EQ(c[i].name, names[i]);
        CHECK_EQ(int(c[0].flags), 2 | 8);
        CHECK_EQ(int(c[1].flags), 2 | 4 | 8);
        CHECK_EQ(int(c[3].flags), 2 | 4); // not covered by the partial initializer
        CHECK_EQ(int(c[4].flags), 1);
        CHECK_EQ(int(c[5].flags), 1 | 4);
        CHECK_EQ(int(c[7].flags), 2);
        CHECK_EQ(int(c[8].flags), 1);
        CHECK_EQ(c[0].initValue, 3u);
        CHECK_EQ(c[2].initValue, 2u);
        CHECK_EQ(std::int32_t(c[9].initValue), -4);
        CHECK(!c[3].initialized);
        CHECK_EQ(s.globals().size(), std::size_t{7});
        CHECK(s.findGlobalVar("p")->origin == GlobalVar::Origin::Parameter);
        CHECK_EQ(s.findGlobalVar("v")->size, 3);
        CHECK(std::ranges::any_of(s.notes(), [](const CompileNote& n) { return n.kind == "partial-array-init"; }));
        CHECK(std::ranges::any_of(s.notes(), [](const CompileNote& n) { return n.kind == "decl-in-function"; }));
        // Initializers are applied at compile time (no code), declarations in functions too.
        Harness h{"int a=3; F(){ int b=7; b=b+1; } main(){ a=a+1; F(); }"};
        CHECK_EQ(h.geti("a"), 3);
        CHECK_EQ(h.geti("b"), 7);
        h.tick();
        h.tick();
        CHECK_EQ(h.geti("a"), 5);
        CHECK_EQ(h.geti("b"), 9); // `int b=7` inside F() is not an assignment
    }
    {
        // Control flow: if/else, else; (empty statement), while, for, break.
        Harness h{"int i, n, m; main(){ for(i=0;i<5;i++){ if(i==3) break; n=n+i; }\n"
                  " while(m<4) m++; if(n==3) ; else n=99; }"};
        h.tick();
        CHECK_EQ(h.geti("n"), 3);
        CHECK_EQ(h.geti("i"), 3);
        CHECK_EQ(h.geti("m"), 4);
    }
    {
        // A function must be defined before it is called (one-pass compiler).
        CHECK(compileError("main(){ F(); } F(){}").has_value());
        CHECK(compileError("F(){} F(){} main(){}").has_value());
    }
}

// Precedence, associativity and evaluation of every operator (§1.5).
void testOperators() {
    Harness h{"int r[24]; int x, y; main(){\n"
              " r[0]=1+2*3; r[1]=1<2<<3; r[2]=2==2<2; r[3]=1|2&0; r[4]=1||0&&0; r[5]=1^3&2;\n"
              " r[6]=10-4-3; r[7]=-2*3; r[8]=!0+1; r[9]=~5; r[10]=7/2; r[11]=-7/2; r[12]=-7%2;\n"
              " r[13]=5/0; r[14]=1<<33; r[15]=-8>>1; r[16]=2&&3; r[17]=0||-3; r[18]=3>2; r[19]=3>=4;\n"
              " r[20]=-3<2; r[21]=4<=4; r[22]=!x=5; r[23]=1-1==0; }"};
    h.tick();
    const std::vector<std::int32_t> expect{
        7,          // * before +
        8,          // (1<2)<<3: comparisons and shifts share a level (C: 1)
        1,          // (2==2)<2 (C: 0)
        0,          // (1|2)&0 (C: 1)
        0,          // (1||0)&&0 (C: 1)
        2,          // (1^3)&2 (C: 3)
        3,          // left associative
        -6, 2, -6,  // unary operators bind to the primary
        3, -3, -1,  // idiv truncates toward zero
        0x0FFFFFFF, // x/0
        2, -4,      // shift counts masked to 5 bits, >> arithmetic
        1, 1, 1, 0, 1, 1,
        0,          // !x=5 is !(x=5)
        1};
    for (std::size_t i = 0; i < expect.size(); ++i)
        if (h.geti("r", int(i)) != expect[i]) {
            ++gromada_test::failures;
            std::fprintf(stderr, "operator test r[%zu] = %d, expected %d\n", i, h.geti("r", int(i)), expect[i]);
        }
    CHECK_EQ(h.geti("x"), 5);

    // x%0, INT_MIN/-1 and INT_MIN%-1 crash the original: an error, or 0/INT_MIN with Ignore.
    CHECK(errorOf([] { Harness{"int x; main(){x=5%0;}"}.tick(); }).has_value());
    CHECK(errorOf([] { Harness{"int x, m=0x80000000; main(){x=m/-1;}"}.tick(); }).has_value());
    {
        ExecConfig cfg;
        cfg.divideError = DivideErrorPolicy::Ignore;
        Harness h2{"int x, y, m=0x80000000; main(){x=5%0; y=m/-1;}", cfg};
        h2.tick();
        CHECK_EQ(h2.geti("x"), 0);
        CHECK_EQ(h2.geti("y"), INT32_MIN);
        CHECK_EQ(h2.host.warnings().size(), std::size_t{2});
    }
}

// && and || evaluate both operands; operands and arguments left to right; a[i]=e evaluates e
// first; the value of an assignment has the type of its right side.
void testEvaluationOrder() {
    {
        Harness h{"int a, b; main(){ a = 0 && GetKey(); b = 1 || GetKey(); }"};
        h.tick();
        CHECK_EQ(h.host.callsTo("GetKey").size(), std::size_t{2}); // no short circuit
        CHECK_EQ(h.geti("a"), 0);
        CHECK_EQ(h.geti("b"), 1);
    }
    {
        Harness h{"int d; main(){ d = GetKey() - GetKey(); }"};
        h.tick();
        CHECK_EQ(h.geti("d"), -1); // 1 - 2
    }
    {
        // (no blank between ']' and '=': the original does not skip it, see testDeclarations...)
        Harness h{"int v[4]; main(){ v[GetKey()]=GetKey(); }"};
        h.tick();
        CHECK_EQ(h.geti("v", 2), 1); // value (1st call) before index (2nd call)
        CHECK_EQ(h.geti("v", 1), 0);
    }
    {
        Harness h{"int n; main(){ Message(n = \"12\"); Print(n); }"};
        h.tick();
        const auto m = h.args("Message");
        CHECK(m.size() == 1 && m[0].isString() && m[0].stringValue() == "12"); // RHS type: string
        CHECK_EQ(h.printed(), std::vector<std::int32_t>{12});
    }
    {
        Harness h{"int a, b, c; main(){ a = b = c = 4; }"};
        h.tick();
        CHECK(h.geti("a") == 4 && h.geti("b") == 4 && h.geti("c") == 4);
    }
    {
        // ++/--: postfix pushes the old value, prefix the new one; nothing happens on strings.
        Harness h{"int i=5, a, b; string s; main(){ s=\"7\"; a=i++; b=--i; s++; }"};
        h.tick();
        CHECK_EQ(h.geti("a"), 5);
        CHECK_EQ(h.geti("b"), 5);
        CHECK_EQ(h.geti("i"), 5);
        CHECK_EQ(h.gets("s"), std::string("7"));
    }
}

// Strings: conversions, +, comparisons, truth, byte writes, cells, static buffers (§1.2, §1.4).
void testStrings() {
    {
        Harness h{"int r[12]; main(){\n"
                  " r[0]=\"5\"+1; r[1]=1+\"5\"; r[2]=5==\"5\"; r[3]=\"5\"==5; r[4]=\"abc\"==0; r[5]=0==\"abc\";\n"
                  " r[6]=\"a\"!=\"b\"; r[7]=\"b\"!=\"a\"; r[8]=\"10\">\"9\"; r[9]=!\"0\"; r[10]=\"abc\"==\"abc\";\n"
                  " r[11]=\"a\"+\"b\"==\"c\"+\"d\"; }"};
        h.tick();
        const std::vector<std::int32_t> expect{6, 6, 1, 1, 1, 0, -1, 1, 1, 1, 1, 1};
        for (std::size_t i = 0; i < expect.size(); ++i)
            if (h.geti("r", int(i)) != expect[i]) {
                ++gromada_test::failures;
                std::fprintf(stderr, "string test r[%zu] = %d, expected %d\n", i, h.geti("r", int(i)), expect[i]);
            }
    }
    {
        // Truth of a string is atoi() != 0.
        Harness h{"int a, b; main(){ if(\"abc\") a=1; if(\"12\") b=1; }"};
        h.tick();
        CHECK_EQ(h.geti("a"), 0);
        CHECK_EQ(h.geti("b"), 1);
    }
    {
        Harness h{"string s, t; int n; main(){ s=\"ab\"+\"cd\"; t=42; n=\"  -12x\"; Message(s+t); }"};
        h.tick();
        CHECK_EQ(h.gets("s"), std::string("abcd"));
        CHECK_EQ(h.gets("t"), std::string("42"));
        CHECK_EQ(h.geti("n"), -12);
        CHECK_EQ(h.args("Message").at(0).toString(), std::string("abcd42"));
    }
    {
        // s[i]=v writes one byte into the buffer: 0 truncates, the old bytes stay behind it.
        Harness h{"string s, t, u; main(){ s=\"maps\"; s[1]='A'; t=s; s[2]=0; u=s; s[2]='x'; }"};
        h.tick();
        CHECK_EQ(h.gets("t"), std::string("mAps"));
        CHECK_EQ(h.gets("u"), std::string("mA"));
        CHECK_EQ(h.gets("s"), std::string("mAxs"));
    }
    {
        // The value written is converted with atoi: s[i]="B" writes 0.
        Harness h{"string s; main(){ s=\"abc\"; s[1]=\"B\"; }"};
        h.tick();
        CHECK_EQ(h.gets("s"), std::string("a"));
    }
    {
        // Reading s[i] does not read a character: it reads the cell i positions after s.
        Harness h{"string s; int a=7, b=9, x, y; main(){ s=\"zz\"; x=s[1]; y=s[2]; }"};
        h.tick();
        CHECK_EQ(h.geti("x"), 7);
        CHECK_EQ(h.geti("y"), 9);
    }
    {
        // Strings longer than 127 bytes get a buffer of their size.
        std::string longText(150, 'q');
        Harness h{"string s, t; int n; main(){ s=\"" + longText + "\"; t=s; n=strlen(t); }"};
        h.tick();
        CHECK_EQ(h.gets("s"), longText);
        CHECK_EQ(h.gets("t"), longText);
        CHECK_EQ(h.geti("n"), 150);
    }
    {
        // The int -> string conversion of a builtin argument and of `+` operands is the decimal
        // text; builtin string results: FRead's static buffer is shared by the whole statement.
        Harness h{"int same, diff; main(){ same = FRead(0) == FRead(0); diff = GetVidName(1) == GetVidName(2); }"};
        h.tick();
        CHECK_EQ(h.geti("same"), 1); // both point to the one FRead buffer
        CHECK_EQ(h.geti("diff"), 0);
    }
    {
        // String arrays: one buffer per element.
        Harness h{"string t[3]; main(){ t[0]=\"a\"; t[2]=\"c\"; t[1]=t[0]+t[2]; }"};
        h.tick();
        CHECK_EQ(h.gets("t", 1), std::string("ac"));
        CHECK_EQ(h.gets("t", 2), std::string("c"));
    }
}

// Arrays and indices: bare name = element 0, no bounds checks (adjacent cells).
void testArrays() {
    Harness h{"int v[2], w=5, x, y; main(){ v=3; x=v[2]; v[4]=8; }"};
    h.tick();
    CHECK_EQ(h.geti("v", 0), 3); // the bare name is element 0
    CHECK_EQ(h.geti("x"), 5);    // v[2] is w
    CHECK_EQ(h.geti("y"), 8);    // v[4] is y
    // Cells outside the array of 2048: ignored with a warning.
    Harness h2{"int v[2], i; main(){ i=5000; v[i]=1; i=v[i]; }"};
    h2.tick();
    CHECK_EQ(h2.geti("i"), 0);
    CHECK_EQ(h2.host.warnings().size(), std::size_t{2});
}

// User functions: parameters are globals, arguments copied, missing ones kept, extra ones
// written into the next cells, return values, stack destruction by calls (§1.6).
void testFunctions() {
    {
        Harness h{"int seen; Show(int a; int b) { Print(a*10+b); }\n"
                  "main(){ Show(1,2); Show(3); seen=a; }"};
        h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{12, 32})); // b kept its value
        CHECK_EQ(h.geti("seen"), 3);                               // parameters are globals
    }
    {
        Harness h{"F(int a) {} int after; main(){ F(1, 2); }"};
        h.tick();
        CHECK_EQ(h.geti("after"), 2); // extra argument stored in the next cell
    }
    {
        Harness h{"int x, y; F(int n) { return(n*2); } main(){ x = F(4); y = F(5) + 1; }"};
        // main ends without return(...): its value is the cell of the return address, 5.
        CHECK_EQ(h.tick().toInt(), 5);
        Harness r{"main(){ return(42); }"};
        CHECK_EQ(r.tick().toInt(), 42);
        CHECK_EQ(h.geti("x"), 8);
        CHECK_EQ(h.geti("y"), 11);
    }
    {
        // `return;` and the end of a function return the cell that held the return address:
        // the code offset after the CALL instruction.
        Harness h{"int x; F(){ return; } main(){ x = F(); }"};
        h.tick();
        const auto& code = h.script.code();
        const std::uint32_t entry = h.script.findFunction("F")->entry;
        std::int32_t after = -1;
        for (std::size_t i = 0; i + 5 <= code.size(); ++i)
            if (code[i] == 0x1E && readU32(code, i + 1) == entry && i != 0) after = std::int32_t(i + 5);
        CHECK(after > 0);
        CHECK_EQ(h.geti("x"), after);
    }
    {
        // ...keeping that cell's flags: after a string argument it is a "string" with a garbage
        // address (the original would crash reading it): "" with a warning here.
        Harness h{"int x=5; G(string p){ return; } main(){ x = G(\"abc\"); }"};
        h.tick();
        CHECK_EQ(h.geti("x"), 0);
        CHECK(!h.host.warnings().empty());
    }
    {
        // A user call destroys the values already pushed by the same statement.
        Harness h{"int x; F(){ return(1); } main(){ x = 1 + F(); }"};
        if (auto e = errorOf([&] { h.tick(); })) CHECK(contains(e->what(), "stack pop, but not push"));
        else CHECK(false);
    }
    {
        // Recursion shares the parameter cells.
        Harness h{"R(int n) { if(n>0) R(n-1); Print(n); } main(){ R(2); }"};
        h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{0, 0, 0}));
    }
    {
        // Endless recursion overflows the 2048 cells.
        Harness h{"R() { R(); } main(){ R(); }"};
        if (auto e = errorOf([&] { h.tick(); })) CHECK(contains(e->what(), "stack overflow"));
        else CHECK(false);
    }
    {
        // Interpreter::call stores the arguments into the parameters.
        Harness h{"int last; Twice(int v) { last=v; return(v*2); } main(){}"};
        const Value args[] = {Value{21}};
        CHECK_EQ(h.vm->call("Twice", args).toInt(), 42);
        CHECK_EQ(h.geti("last"), 21);
        CHECK_EQ(h.vm->call("Twice").toInt(), 42); // missing argument: keeps 21
    }
    {
        ExecConfig cfg;
        cfg.traceUserFunctions = true;
        Harness h{"F(int a; string b){} main(){ F(1, \"x\"); }", cfg};
        h.tick();
        const auto& calls = h.host.calls();
        CHECK(calls.size() == 2 && calls[0].name == "main" && calls[1].name == "F");
        if (calls.size() == 2) CHECK(calls[1].args == (std::vector<Value>{Value{1}, Value{"x"}}));
    }
}

// iff: the code patch of the original (§1.7).
void testIff() {
    {
        // `iff(1) {...; return;}` blocks run one per frame: a sequence.
        Harness h{"main(){ iff(1) { Print(1); return; } iff(1) { Print(2); return; } Print(3); }"};
        for (int i = 0; i < 4; ++i) h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{1, 2, 3, 3}));
        CHECK_EQ(h.vm->iffFireCount(0), 1u);
        CHECK_EQ(h.vm->iffFireCount(1), 1u);
    }
    {
        // After firing, the condition is no longer evaluated.
        Harness h{"main(){ iff(GetKey()) Print(7); }"};
        for (int i = 0; i < 3; ++i) h.tick();
        CHECK_EQ(h.host.callsTo("GetKey").size(), std::size_t{1});
        CHECK_EQ(h.printed(), std::vector<std::int32_t>{7});
    }
    {
        // Until it fires the condition is evaluated every time.
        Harness h{"int go; main(){ iff(go) Print(1); }"};
        h.tick();
        h.tick();
        h.vm->set("go", Value{1});
        h.tick();
        h.tick();
        CHECK_EQ(h.printed(), std::vector<std::int32_t>{1});
    }
    {
        // One site, also in a loop and in a function called from several places.
        Harness h{"int i; F(){ iff(1) Print(5); } main(){ for(i=0;i<3;i++) iff(1) Print(9); F(); F(); }"};
        h.tick();
        h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{9, 5}));
    }
    {
        // Reached by the JMP at the end of an if branch: the JMP is patched, and the iff fires a
        // second time when reached through the else branch.
        const char* src = "int k, go; main(){ if(k==1) Print(10); else Print(20); iff(go) Print(99); }";
        Harness h{src};
        CHECK(h.script.iffSites().at(0).reachableByJump);
        h.vm->set("go", Value{1});
        h.vm->set("k", Value{1});
        h.tick(); // 10 99 (patches the if branch's JMP)
        h.tick(); // 10    (the JMP now skips the iff)
        h.vm->set("k", Value{2});
        h.tick(); // 20 99 (second firing: patches the iff itself)
        h.tick(); // 20
        h.vm->set("k", Value{1});
        h.tick(); // 10
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{10, 99, 10, 20, 99, 20, 10}));
        CHECK_EQ(h.vm->iffFireCount(0), 2u);
        // OncePerSite: exactly once.
        ExecConfig once;
        once.iffMode = IffMode::OncePerSite;
        Harness o{src, once};
        o.vm->set("go", Value{1});
        o.vm->set("k", Value{1});
        o.tick();
        o.tick();
        o.vm->set("k", Value{2});
        o.tick();
        CHECK_EQ(o.printed(), (std::vector<std::int32_t>{10, 99, 10, 20}));
        // PlainIf: every time.
        ExecConfig plain;
        plain.iffMode = IffMode::PlainIf;
        Harness p{src, plain};
        p.vm->set("go", Value{1});
        p.tick();
        p.tick();
        CHECK_EQ(p.printed(), (std::vector<std::int32_t>{20, 99, 20, 99}));
    }
    {
        // Reached by `break`: the break's JMP is patched.
        Harness h{"int i, stop; main(){ for(i=0;i<3;i++){ if(i==stop) break; } iff(1) Print(7); Print(i); }"};
        CHECK(h.script.iffSites().at(0).reachableByJump);
        h.vm->set("stop", Value{1});
        h.tick(); // break -> iff fires (7), patches the break: 7 1
        h.tick(); // break now jumps past the iff: 1
        h.vm->set("stop", Value{9});
        h.tick(); // loop ends normally -> iff reached through the for's JZ: fires again: 7 3
        h.tick(); // 3
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{7, 1, 1, 7, 3, 3}));
    }
    {
        // iff ... else: valid; once fired, the else branch runs every time.
        Harness h{"int go; main(){ iff(go) Print(1); else Print(2); }"};
        h.tick();
        h.vm->set("go", Value{1});
        h.tick();
        h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{2, 1, 2}));
        CHECK(std::ranges::any_of(h.script.notes(), [](const CompileNote& n) { return n.kind == "iff-else"; }));
    }
    {
        // A new map (new Interpreter, or resetIffs) restores the code.
        Harness h{"main(){ iff(1) Print(1); }"};
        h.tick();
        h.tick();
        CHECK(std::ranges::equal(h.vm->code(), h.script.code()) == false);
        h.vm->resetIffs();
        CHECK(std::ranges::equal(h.vm->code(), h.script.code()));
        h.tick();
        RecordingHost other;
        Interpreter fresh{h.script, other};
        fresh.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{1, 1}));
        CHECK_EQ(other.callsTo("Print").size(), std::size_t{1});
    }
}

// Builtins: arity of the original's handler, pushed values, first declaration wins,
// ID truncated to a byte, unknown opcodes, stack errors (§1.9, §1.11).
void testBuiltins() {
    {
        // The parameter text of an extern is ignored: the handler pops what it needs.
        Harness g{"extern Shown() 66; main(){ Shown(5); }"};
        g.tick();
        // The host sees the first declaration of opcode 66 (Print, from kExterns) with 1 value.
        CHECK_EQ(g.printed(), std::vector<std::int32_t>{5});
        CHECK_EQ(g.host.calls().at(0).id, 66);
    }
    {
        // First declaration wins; the ID is truncated to a byte.
        Harness h{"extern Stop() 134; extern Stop(int n) 136; extern Odd(int i) 322; main(){ Stop(); Odd(4); }"};
        h.tick();
        CHECK_EQ(h.host.calls().at(0).id, 134);
        CHECK_EQ(h.host.calls().at(1).id, 66);
        CHECK(std::ranges::any_of(h.script.notes(), [](const CompileNote& n) { return n.kind == "duplicate-extern"; }));
        CHECK(compileError("extern Neg() -1; main(){}").has_value());
    }
    {
        // An extern ID below 64 is a VM opcode: 3 is ADD.
        Harness h{"extern Plus() 3; int x; main(){ x = Plus(2, 3); }"};
        h.tick();
        CHECK_EQ(h.geti("x"), 5);
    }
    {
        // Opcodes without handler end the frame with "Unknown command".
        Harness h{"extern Nothing() 200; main(){ Print(1); Nothing(); Print(2); }"};
        h.tick();
        h.tick();
        CHECK_EQ(h.printed(), (std::vector<std::int32_t>{1, 1}));
        CHECK(!h.host.warnings().empty() && contains(h.host.warnings()[0], "Unknown command 200"));
    }
    {
        // Extra arguments stay on the stack (the handler pops the last ones): stack error at ';'.
        Harness h{"main(){ Print(1, 2, 3); }"};
        h.tick();
        CHECK_EQ(h.printed(), std::vector<std::int32_t>{3});
        CHECK(!h.host.warnings().empty() && contains(h.host.warnings()[0], "stack error"));
    }
    {
        // Builtins that push nothing cannot be used as values.
        Harness h{"int x; main(){ x = PlaySFX(1); }"};
        CHECK(errorOf([&] { h.tick(); }).has_value());
        Harness g{"int x, y; main(){ x = GetNetInfo(1); y = GetNetInfo(9); }"};
        CHECK(errorOf([&] { g.tick(); }).has_value());
        CHECK_EQ(g.geti("x"), 100);
    }
    {
        // Arguments arrive as they are (an int where the declaration says string).
        Harness h{"main(){ Message(12); Message(\"t\"); }"};
        h.tick();
        const auto m = h.args("Message");
        CHECK(m.size() == 2 && m[0].isInt() && m[1].isString());
    }
    {
        // Builtin results: ints and strings.
        Harness h{"string s; int n; main(){ s = GetVidName(3); n = strlen(s) + GetKey(); }"};
        h.tick();
        CHECK_EQ(h.gets("s"), std::string("vid3"));
        CHECK_EQ(h.geti("n"), 5);
    }
    {
        CHECK(knownBuiltinPushes(toInt(BuiltinId::GetTime), {}));
        CHECK(!knownBuiltinPushes(toInt(BuiltinId::Load), {}));
        CHECK_EQ(knownBuiltinArity(toInt(BuiltinId::Action)), std::size_t{4});
        CHECK_EQ(knownBuiltinArity(toInt(BuiltinId::StopSFX)), std::size_t{0});
        CHECK_EQ(knownBuiltinArity(toInt(BuiltinId::StopSFXOne)), std::size_t{1});
        CHECK_EQ(knownBuiltinRegistry().size(), kKnownBuiltins.size());
        for (std::size_t i = 0; i < kKnownBuiltins.size(); ++i) CHECK_EQ(toInt(kKnownBuiltins[i].id), std::int32_t(64 + i));
    }
}

// Runtime limits and errors.
void testLimits() {
    {
        ExecConfig cfg;
        cfg.maxSteps = 10000;
        Harness h{"main(){ while(1) ; }", cfg};
        if (auto e = errorOf([&] { h.tick(); })) CHECK(contains(e->what(), "step limit"));
        else CHECK(false);
    }
    {
        ExecConfig cfg;
        cfg.outOfRange = OutOfRangePolicy::Error;
        Harness h{"int v[2], i; main(){ i=5000; v[i]=1; }", cfg};
        CHECK(errorOf([&] { h.tick(); }).has_value());
    }
    {
        // `if` conditions and statements with stray values: logged, not fatal.
        Harness h{"int x; main(){ for(x=0; x<1; x++) ; }"};
        h.tick();
        CHECK(h.host.warnings().empty()); // the for initialization's value is allowed
    }
}

// Map changes: cells survive by index; initializers re-applied; strings reset (§1.3).
void testCarryOver() {
    RecordingHost host;
    const Script a = Script::compileText(
        "extern Print(int i) 66;\n"
        "int keep, init=5, arr[3]={1}; string s; int extra;\n"
        "main(){ keep=11; init=6; arr[0]=0; arr[1]=22; arr[2]=33; s=\"text\"; extra=4; Print(55); }");
    Interpreter va{a, host};
    va.tick();
    // Different names, same cells (the header is always first in the game): by cell index.
    const Script b = Script::compileText(
        "int k2, init=5, arr[3]={1}; int wasString; string s2; int ret, top;\n"
        "main(){}");
    Interpreter vb{b, host};
    vb.inheritGlobals(va.snapshot());
    CHECK_EQ(vb.get("k2").toInt(), 11);        // cell 0: kept although renamed
    CHECK_EQ(vb.get("init").toInt(), 5);       // initializer applied again
    CHECK_EQ(vb.get("arr", 0).toInt(), 1);     // initialized element
    CHECK_EQ(vb.get("arr", 1).toInt(), 22);    // not covered by the partial initializer: kept
    CHECK_EQ(vb.get("arr", 2).toInt(), 33);
    CHECK_EQ(vb.get("wasString").toInt(), 0);  // was the previous script's string: freed to 0
    CHECK_EQ(vb.get("s2").toString(), std::string{}); // strings restart from ""
    // Cells past A's globals were A's stack: cell 7 got main's value (the return address 5),
    // cell 8 the last value pushed (Print's argument).
    CHECK_EQ(vb.get("ret").toInt(), 5);
    CHECK_EQ(vb.get("top").toInt(), 55);
    // Demo snapshots: every int cell in order, strings skipped.
    const auto ints = vb.intGlobalValues();
    CHECK_EQ(ints.size(), std::size_t{8});
    CHECK_EQ(ints[0], 11u);
    std::vector<std::uint32_t> demo(8, 0);
    demo[0] = 123;
    demo[7] = 9;
    CHECK(vb.setIntGlobalValues(demo));
    CHECK_EQ(vb.get("k2").toInt(), 123);
    CHECK_EQ(vb.get("top").toInt(), 9);
    CHECK(!vb.setIntGlobalValues(std::vector<std::uint32_t>(7, 0)));
    // The old by-name policy is still available.
    const Script c = Script::compileText("int keep, init=1; string s; main(){}");
    Interpreter vc{c, host};
    CarryOverPolicy byName;
    byName.mode = CarryOverPolicy::Mode::ByName;
    vc.inheritGlobals(va.snapshot(), byName);
    CHECK_EQ(vc.get("keep").toInt(), 11);
    CHECK_EQ(vc.get("init").toInt(), 1);
    // A fresh game: every cell 0.
    Interpreter vd{b, host};
    CHECK_EQ(vd.get("k2").toInt(), 0);
    CHECK_EQ(vd.get("top").toInt(), 0);
}

void testValueConversions() {
    CHECK_EQ(stringToInt(" \t\v-42abc"), -42);
    CHECK_EQ(stringToInt("+7"), 7);
    CHECK_EQ(stringToInt("x1"), 0);
    CHECK_EQ(stringToInt("4294967297"), 1); // wraps like the CRT
    CHECK(Value{"12"}.truthy());
    CHECK(!Value{"abc"}.truthy());
    CHECK_EQ(Value{-5}.toString(), std::string("-5"));
}

void testSimulatedWorld() {
    SimulatedWorld w;
    RecordingHost host;
    host.setResponder([&](const BuiltinInfo& b, std::span<const Value> a) { return w(b, a); });
    const Script s = Script::compileText(withExterns(
        "extern Random(int max) 90; extern Create(int nvid,int x,int y,int z) 68; extern GetTime() 69;\n"
        "int r, lo=99, hi, u, i; main(){ for(i=0;i<200;i++){ r=Random(3); if(r<lo) lo=r; if(r>hi) hi=r; } u=Create(1,2,3,4); }"));
    Interpreter vm{s, host};
    vm.tick();
    CHECK_EQ(vm.get("lo").toInt(), 0);
    CHECK_EQ(vm.get("hi").toInt(), 3); // rand() % (max+1): 0..max
    CHECK(vm.get("u").toInt() != 0);
}

} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"bytecode", testBytecode},           {"lexical", testLexical},
        {"declarations", testDeclarationsAndStatements},
        {"operators", testOperators},         {"evaluation order", testEvaluationOrder},
        {"strings", testStrings},             {"arrays", testArrays},
        {"functions", testFunctions},         {"iff", testIff},
        {"builtins", testBuiltins},           {"limits", testLimits},
        {"carry over", testCarryOver},        {"values", testValueConversions},
        {"simulated world", testSimulatedWorld},
    };
    for (const auto& [name, fn] : tests) {
        try {
            fn();
        } catch (const std::exception& e) {
            ++gromada_test::failures;
            std::fprintf(stderr, "test '%s' threw: %s\n", name, e.what());
        }
    }
    return gromada_test::finish();
}
