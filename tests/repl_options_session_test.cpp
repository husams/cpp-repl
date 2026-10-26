#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>
#include <unistd.h>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "session_commands.hpp"
#include "startup_files.hpp"

namespace example {
namespace {

TEST(ReplOptionsSession, QueriesAndAssignmentsShareTheInterpreterOptions) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libraries;
  std::ostringstream output;
  EXPECT_TRUE(execute_command(libraries, *cpp, parse_command("$print_values"), output).ok);
  EXPECT_EQ(output.str(), "print_values = true\n");
  output.str("");
  EXPECT_TRUE(
      execute_command(libraries, *cpp, parse_command("%set print_values=false"), output).ok);
  EXPECT_FALSE(cpp->options().print_values);
  EXPECT_EQ(output.str(), "print_values = false\n");
  output.str("");
  EXPECT_TRUE(execute_command(libraries, *cpp, parse_command("$print_values"), output).ok);
  EXPECT_EQ(output.str(), "print_values = false\n");
  EXPECT_TRUE(cpp->options().keep_hints);
  EXPECT_FALSE(
      execute_command(libraries, *cpp, parse_command("%set keep_hints = maybe"), output).ok);
  EXPECT_TRUE(cpp->options().keep_hints);
  EXPECT_FALSE(execute_command(libraries, *cpp, parse_command("$missing_option"), output).ok);
  EXPECT_NE(output.str().find("unknown option"), std::string::npos);
}

TEST(ReplOptionsSession, StartupFilesCanConfigureBothOptions) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("cpp-repl-option-startup-" + std::to_string(getpid()));

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};

  std::filesystem::create_directories(directory);
  std::ofstream(directory / "10-options.repl")
      << "%set print_values = false\n%set keep_hints = false\n";
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libraries;
  std::ostringstream output;
  const auto loaded = load_startup_files(libraries, *cpp, output, directory.string());
  EXPECT_EQ(loaded.commands, 2u);
  EXPECT_EQ(loaded.failed, 0u);
  EXPECT_FALSE(cpp->options().print_values);
  EXPECT_FALSE(cpp->options().keep_hints);
}

} // namespace
} // namespace example
