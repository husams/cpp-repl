#include "debugger.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

TEST(Debugger, KeepsPromptSourcesAndCleansUpAtShutdown) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error, true);
  ASSERT_TRUE(cpp) << error;
  const auto directory = cpp->debug_source_directory();
  ASSERT_TRUE(std::filesystem::is_directory(directory));
  const std::string code = "int debug_twice(int value) {\n  return value * 2;\n}";
  ASSERT_TRUE(cpp->execute(code));
  std::ifstream source(directory + "/input-1.cpp");
  const std::string saved((std::istreambuf_iterator<char>(source)), {});
  EXPECT_EQ(saved, code + '\n');
  EXPECT_EQ(cpp->evaluate("debug_twice(21)"), "(int) 42\n");
  EXPECT_FALSE(cpp->execute("this is invalid C++"));
  EXPECT_EQ(cpp->evaluate("debug_twice(3)"), "(int) 6\n");
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "debug_twice(int value)", "return value + 1;", error))
      << error;
  EXPECT_EQ(cpp->evaluate("debug_twice(3)"), "(int) 4\n");
  ASSERT_TRUE(cpp->restore("debug_twice(int)", error)) << error;
  EXPECT_EQ(cpp->evaluate("debug_twice(3)"), "(int) 6\n");
  cpp.reset();
  EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(Debugger, NormalSessionsDoNotRecordSources) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_TRUE(cpp) << error;
  EXPECT_TRUE(cpp->debug_source_directory().empty());
}

TEST(Debugger, SourceWriteFailureDoesNotExecuteInput) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error, true);
  ASSERT_TRUE(cpp) << error;
  const auto directory = cpp->debug_source_directory();
  std::filesystem::remove_all(directory);
  EXPECT_FALSE(cpp->execute("int should_not_exist = 42;"));
  std::filesystem::create_directory(directory);
  EXPECT_FALSE(cpp->evaluate("should_not_exist"));
  EXPECT_EQ(cpp->evaluate("6 * 7"), "(int) 42\n");
}

}  // namespace
}  // namespace example
