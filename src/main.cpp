#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "debugger.hpp"
#include "library_manager.hpp"
#include "repl.hpp"
#include "replxx.hxx"
#include "startup_files.hpp"

namespace {

constexpr const char* kHistoryFile = "./.replxx_history";
constexpr const char* kPrompt = "\033[1;32mcpp-repl\033[0m> ";

}  // namespace

// Usage: cpp-repl [--debug | --debug-info] [compiler flags...]
int main(int argc, char** argv) {
  const bool debug = argc > 1 && std::string(argv[1]) == "--debug";
  const bool debug_info = argc > 1 && std::string(argv[1]) == "--debug-info";
  const std::vector<std::string> flags(argv + (debug || debug_info ? 2 : 1), argv + argc);
  if (debug) return example::launch_debugger(argv[0], flags);
  std::string error;
  const std::unique_ptr<example::CppInterpreter> cpp =
      example::CppInterpreter::create(flags, error, debug_info);
  if (!cpp) {
    std::cerr << "cpp-repl: cannot create the interpreter: " << error << '\n';
    return EXIT_FAILURE;
  }

  example::LibraryManager libs;
  libs.set_load_hook([&cpp](const std::string& path) { return cpp->load_library(path); });

  replxx::Replxx rx;
  example::configure(rx, *cpp);
  rx.history_load(kHistoryFile);

  std::cout << "cpp-repl (" << example::CppInterpreter::version()
            << "). Type %help for commands.\n";
  if (!cpp->compilation_database_path().empty()) {
    std::cout << "Compile options: " << cpp->compilation_database_path() << '\n';
  }
  if (example::load_startup_directories(libs, *cpp, std::cout).quit) return EXIT_SUCCESS;

  std::string pending;

  for (;;) {
    errno = 0;
    const char* raw = rx.input(kPrompt);

    if (raw == nullptr) {
      if (errno == EAGAIN) {
        // Ctrl+C: drop the current line and keep going.
        continue;
      }
      // Ctrl+D on an empty line, or end of piped input.
      if (!pending.empty()) example::execute(libs, *cpp, pending);
      std::cout << '\n';
      break;
    }

    std::string line(raw);
    // replxx reads pipes one physical line at a time. Accumulate open command
    // blocks there too, using the same delimiter rules as interactive Enter.
    if (!pending.empty()) line = pending + '\n' + line;
    if (!isatty(STDIN_FILENO) && example::wants_more_input(line) &&
        example::is_command_line(line)) {
      pending = std::move(line);
      continue;
    }
    pending.clear();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;

    rx.history_add(line);
    if (!example::execute(libs, *cpp, line)) break;
  }

  rx.history_save(kHistoryFile);
  return EXIT_SUCCESS;
}
