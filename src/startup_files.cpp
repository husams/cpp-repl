#include "startup_files.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "inspection_output.hpp"
#include "library_manager.hpp"
#include "session_commands.hpp"

namespace example {
namespace {

namespace fs = std::filesystem;

struct StartupScript {
  fs::path path;
  std::string order;
};

std::optional<std::string> script_order(const fs::path& path) {
  const auto name = path.filename().string();
  const auto dash = name.find('-');
  if (path.extension() != ".repl" || dash == std::string::npos || dash == 0 ||
      dash + 1 >= name.size() - 5)
    return std::nullopt;
  auto order = name.substr(0, dash);
  if (!std::all_of(order.begin(), order.end(),
                   [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  const auto first = order.find_first_not_of('0');
  return first == std::string::npos ? "0" : order.substr(first);
}

void load_script(LibraryManager& libs, CppInterpreter& cpp,
                 std::ostream& output, const fs::path& path,
                 StartupFilesResult& result) {
  const bool colors = inspection_colors_enabled();
  auto fail = [&](unsigned line, const std::string& message) {
    ++result.failed;
    output << color_inspection("error: " + path.string() + ":" +
                                   std::to_string(line) + ": " + message + '\n',
                               colors);
  };
  std::ifstream input(path);
  if (!input) {
    fail(1, "cannot open startup script");
    return;
  }
  ++result.loaded;
  std::string line;
  std::string pending;
  unsigned number = 0;
  unsigned entry_number = 0;
  while (std::getline(input, line)) {
    ++number;
    if (!pending.empty()) {
      line = pending + '\n' + line;
      pending.clear();
    } else {
      const auto begin = line.find_first_not_of(" \t\r");
      if (begin == std::string::npos) continue;
      entry_number = number;
    }
    if (wants_more_input(line)) {
      pending = std::move(line);
      continue;
    }
    if (is_command_line(line)) {
      const auto command = parse_command(line);
      const auto executed = execute_command(libs, cpp, command, output);
      if (executed.ok)
        ++result.commands;
      else
        fail(entry_number, "startup command failed: " + command.name);
      if (!executed.keep_running) {
        result.quit = true;
        break;
      }
    } else if (!cpp.execute(line)) {
      fail(entry_number, "startup C++ input failed");
    }
  }
  if (!pending.empty())
    fail(entry_number, is_command_line(pending) ? "unterminated cpp` block"
                                                : "incomplete C++ input");
  if (input.bad()) fail(number, "could not finish reading startup script");
  output << color_inspection("preloaded: " + path.string() + '\n', colors);
}

}  // namespace

StartupFilesResult load_startup_files(LibraryManager& libs, CppInterpreter& cpp,
                                      std::ostream& output,
                                      const std::string& config) {
  StartupFilesResult result;
  const bool colors = inspection_colors_enabled();
  auto fail = [&](const std::string& message) {
    ++result.failed;
    output << color_inspection("error: " + message + '\n', colors);
  };
  std::error_code ec;
  const auto path = fs::absolute(expand_home(config), ec).lexically_normal();
  if (ec) {
    fail("cannot resolve startup directory: " + config + ": " + ec.message());
    return result;
  }
  result.found = fs::exists(path, ec);
  if (!result.found && !ec) return result;
  if (ec) {
    fail("cannot read startup directory: " + path.string() + ": " +
         ec.message());
    return result;
  }
  if (!fs::is_directory(path, ec)) {
    fail("startup path must be a directory: " + path.string() +
         "; put commands and C++ in <number>-<name>.repl files inside it");
    return result;
  }

  std::vector<StartupScript> scripts;
  fs::directory_iterator entry(path, ec);
  const fs::directory_iterator end;
  for (; !ec && entry != end; entry.increment(ec)) {
    const auto order = script_order(entry->path());
    if (!order) continue;
    std::error_code status_error;
    if (entry->is_regular_file(status_error)) {
      scripts.push_back({entry->path(), *order});
    } else if (status_error) {
      fail("cannot read startup entry: " + entry->path().string() + ": " +
           status_error.message());
    }
  }
  if (ec)
    fail("cannot list startup directory: " + path.string() + ": " +
         ec.message());
  std::sort(scripts.begin(), scripts.end(),
            [](const StartupScript& left, const StartupScript& right) {
              if (left.order.size() != right.order.size())
                return left.order.size() < right.order.size();
              if (left.order != right.order) return left.order < right.order;
              return left.path.filename() < right.path.filename();
            });
  for (const auto& script : scripts) {
    load_script(libs, cpp, output, script.path, result);
    if (result.quit) break;
  }
  return result;
}

}  // namespace example
