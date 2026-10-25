#pragma once

#include <iosfwd>
#include <string>

namespace example {

class CppInterpreter;
class LibraryManager;

struct StartupFilesResult {
  bool found = false;
  unsigned loaded = 0;  // scripts opened, including scripts with input errors
  unsigned commands = 0;
  unsigned failed = 0;
  bool quit = false;
};

// Load <number>-<name>.repl files from a directory in numeric order, breaking
// ties by filename. Scripts contain C++ input and %commands, as at the prompt;
// relative paths use the REPL's working directory. No recursive traversal.
// Missing directories are silent; input failures do not stop later scripts.
StartupFilesResult load_startup_files(
    LibraryManager& libs, CppInterpreter& cpp, std::ostream& output,
    const std::string& config = "~/.cpp-repl");

// Load user scripts first, then the nearest .cpp-repl in the startup directory
// or its ancestors. The user directory is never loaded twice. Project scripts
// run relative to their project root; the caller's working directory is
// restored.
StartupFilesResult load_startup_directories(
    LibraryManager& libs, CppInterpreter& cpp, std::ostream& output,
    const std::string& user_config = "~/.cpp-repl");

}  // namespace example
