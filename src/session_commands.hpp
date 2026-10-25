#pragma once

#include <iosfwd>

namespace example {

class CppInterpreter;
class LibraryManager;
struct ParsedCommand;

struct CommandResult {
  bool keep_running = true;
  bool ok = true;
};

// Shared command execution for interactive input and startup configuration.
// C++ inputs are handled separately by the interactive REPL.
CommandResult execute_command(LibraryManager& libs, CppInterpreter& cpp,
                              const ParsedCommand& command, std::ostream& out);

}  // namespace example
