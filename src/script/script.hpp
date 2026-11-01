#pragma once
// Gromada script module (header-only): lexer, preprocessor, parser, linker and interpreter of
// the game's .INI scripts. See docs/script-language.md.
//
// Typical use:
//   FileSystemSourceLoader loader{gameDir};
//   Script script = Script::compile(loader, "maps/levels/05.INI");
//   MyHost host;                       // implements ScriptHost::callBuiltin (dispatch on id)
//   Interpreter vm{script, host};
//   vm.inheritGlobals(previousScriptGlobals);   // optional
//   each logic tick: vm.tick();
#include "script/diagnostics.hpp"
#include "script/value.hpp"
#include "script/source.hpp"
#include "script/lexer.hpp"
#include "script/preprocessor.hpp"
#include "script/ast.hpp"
#include "script/parser.hpp"
#include "script/builtins.hpp"
#include "script/builtin_ids.hpp"
#include "script/compiler.hpp"
#include "script/interpreter.hpp"
