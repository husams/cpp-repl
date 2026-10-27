#include "debugger.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <utility>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#ifndef CPP_REPL_LLDB_EXECUTABLE
#define CPP_REPL_LLDB_EXECUTABLE "lldb"
#endif

namespace example {

int launch_debugger(const char* executable, const std::vector<std::string>& flags) {
  const char* override = std::getenv("CPP_REPL_LLDB");
  const std::string lldb = override && *override ? override : CPP_REPL_LLDB_EXECUTABLE;
  // Resolve our own executable independently of PATH and the current directory.
  const auto self =
      llvm::sys::fs::getMainExecutable(executable, reinterpret_cast<void*>(&launch_debugger));
  const auto script = (std::filesystem::path(self).parent_path() / "cpp_repl_lldb.py").string();
  std::string quoted_script = "\"";
  for (const char c : script) {
    if (c == '\\' || c == '"') quoted_script += '\\';
    quoted_script += c;
  }
  quoted_script += '"';
  std::error_code script_error;
  if (!std::filesystem::is_regular_file(script, script_error)) {
    std::cerr << "cpp-repl: missing LLDB stepping support: " << script
              << "; reinstall cpp-repl with its companion script\n";
    return EXIT_FAILURE;
  }
  std::vector<std::string> arguments = {
      lldb,          "--no-lldbinit",
      "-o",          "settings set plugin.jit-loader.gdb.enable on",
      "-o",          "settings set target.disable-aslr false",
      "-o",          "command script import " + quoted_script,
      "--",          self,
      "--debug-info"};
  arguments.insert(arguments.end(), flags.begin(), flags.end());
  std::vector<char*> argv;
  for (auto& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::cout << "LLDB: set breakpoints with 'breakpoint set -n FUNCTION', then 'run'.\n"
               "At a stop: 'next', 'step', 'finish', 'frame variable', 'bt', 'continue'.\n"
            << std::flush;
  execvp(argv.front(), argv.data());
  const int failure = errno;
  std::cerr << "cpp-repl: cannot start LLDB (" << lldb << "): " << std::strerror(failure)
            << "; install LLDB or set CPP_REPL_LLDB\n";
  return EXIT_FAILURE;
}

DebugSources::DebugSources(FactoryKey, std::string directory) : directory_(std::move(directory)) {}

std::unique_ptr<DebugSources> DebugSources::create(std::string& error) {
  llvm::SmallString<128> directory;
  if (auto failure = llvm::sys::fs::createUniqueDirectory("cpp-repl-debug", directory)) {
    error = "cannot create debugger source directory: " + failure.message();
    return nullptr;
  }
  auto result = std::make_unique<DebugSources>(FactoryKey{}, directory.str().str());
  if (auto failure = llvm::sys::fs::setPermissions(directory, llvm::sys::fs::owner_all)) {
    error = "cannot protect debugger source directory: " + failure.message();
    return nullptr;
  }
  return result;
}

DebugSources::~DebugSources() {
  std::error_code ignored;
  std::filesystem::remove_all(directory_, ignored);
}

bool DebugSources::prepare(const std::string& code, std::string& input, std::string& error) {
  const auto path = directory_ + "/input-" + std::to_string(++next_) + ".cpp";
  std::error_code failure;
  llvm::raw_fd_ostream source(path, failure);
  if (failure) {
    error = "cannot save debugger source: " + failure.message();
    return false;
  }
  source << code << '\n';
  source.close();
  if (source.has_error()) {
    error = "cannot write debugger source: " + source.error().message();
    source.clear_error();
    return false;
  }
  std::string escaped;
  for (const char c : path) {
    if (c == '\\' || c == '"') escaped += '\\';
    escaped += c;
  }
  input = "#line 1 \"" + escaped + "\"\n" + code;
  return true;
}

}  // namespace example
