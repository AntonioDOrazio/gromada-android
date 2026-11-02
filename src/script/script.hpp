#pragma once
// Gromada script module (header-only): a faithful reimplementation of the original game's
// script compiler and bytecode VM for the .INI scripts. See docs/script-language.md.
//
// Typical use:
//   FileSystemSourceLoader loader{gameDir};
//   Script script = Script::compile(loader, "maps/levels/05.INI");
//   MyHost host;                       // implements ScriptHost::callBuiltin (dispatch on id)
//   Interpreter vm{script, host};
//   vm.inheritGlobals(previousInterpreter.snapshot());   // cells carried over a map load
//   each frame: vm.tick();
#include "script/diagnostics.hpp"
#include "script/value.hpp"
#include "script/source.hpp"
#include "script/program.hpp"
#include "script/builtins.hpp"
#include "script/builtin_ids.hpp"
#include "script/bytecode.hpp"
#include "script/compiler.hpp"
#include "script/memory.hpp"
#include "script/interpreter.hpp"
