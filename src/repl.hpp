#pragma once

#include <string>

#include "cpp_interpreter.hpp"
#include "library_manager.hpp"
#include "replxx.hxx"

namespace example {

// Wires completion (C++ completion comes from `cpp`), hints, highlighting and
// key bindings into `rx`.
void configure(replxx::Replxx& rx, CppInterpreter& cpp);

// Executes one submitted input: a %command, or C++ run by `cpp`. Returns false
// when the REPL should exit.
bool execute(LibraryManager& libs, CppInterpreter& cpp, const std::string& line);

}  // namespace example
