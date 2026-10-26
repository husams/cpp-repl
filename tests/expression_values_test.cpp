#include <cerrno>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

class StdoutCapture {
public:
  StdoutCapture() {
    int descriptors[2];
    if (pipe(descriptors) != 0) {
      return;
    }
    read_fd_ = descriptors[0];
    saved_stdout_ = dup(STDOUT_FILENO);
    if (saved_stdout_ < 0 || dup2(descriptors[1], STDOUT_FILENO) < 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      read_fd_ = -1;
      return;
    }
    close(descriptors[1]);
  }

  ~StdoutCapture() { (void)finish(); }

  std::string finish() {
    if (read_fd_ < 0) {
      return {};
    }
    std::fflush(stdout);
    (void)dup2(saved_stdout_, STDOUT_FILENO);
    close(saved_stdout_);
    saved_stdout_ = -1;
    std::string output;
    char buffer[256];
    ssize_t count;
    while ((count = read(read_fd_, buffer, sizeof(buffer))) > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
    }
    close(read_fd_);
    read_fd_ = -1;
    return output;
  }

private:
  int read_fd_ = -1;
  int saved_stdout_ = -1;
};

TEST(ExpressionValuesTest, PrintsTopLevelSemicolonExpressionsOnce) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  EXPECT_TRUE(cpp->options().print_values);

  StdoutCapture captured;
  ASSERT_TRUE(cpp->execute("int add(int a, int b) { return a + b; }"));
  ASSERT_TRUE(cpp->execute("add(10, 20);"));
  ASSERT_TRUE(cpp->execute("1 + 2;"));
  ASSERT_TRUE(cpp->execute("1 + 4"));
  ASSERT_TRUE(cpp->execute("#define VALUE_FROM_MACRO 42\nVALUE_FROM_MACRO;"));
  ASSERT_TRUE(cpp->execute("int expression_effects = 0;"));
  ASSERT_TRUE(cpp->execute("++expression_effects;"));
  ASSERT_TRUE(cpp->execute("++expression_effects; ++expression_effects;"));
  const std::string output = captured.finish();
  EXPECT_EQ(output, "(int) 30\n(int) 3\n(int) 5\n(int) 42\n(int) 1\n(int) 3\n");
  EXPECT_EQ(cpp->evaluate("expression_effects"), "(int) 3\n");
}

TEST(ExpressionValuesTest, OmitsValuesForVoidDeclarationsAndControlFlow) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int control_effects = 0; void notify() { ++control_effects; }"));

  StdoutCapture captured;
  ASSERT_TRUE(cpp->execute("notify();"));
  ASSERT_TRUE(cpp->execute("int declared_value = 8;"));
  ASSERT_TRUE(cpp->execute("if (true) { ++control_effects; }"));
  EXPECT_TRUE(captured.finish().empty());
  EXPECT_EQ(cpp->evaluate("control_effects"), "(int) 2\n");
}

TEST(ExpressionValuesTest, OptionCanSuppressAndRestoreSemicolonValues) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;

  StdoutCapture captured;
  cpp->options().print_values = false;
  ASSERT_TRUE(cpp->execute("int suppressed_effects = 0;"));
  ASSERT_TRUE(cpp->execute("++suppressed_effects;"));
  ASSERT_TRUE(cpp->execute("4 * 5;"));
  ASSERT_TRUE(cpp->execute("++suppressed_effects"));
  cpp->options().print_values = true;
  ASSERT_TRUE(cpp->execute("6 * 7;"));
  EXPECT_EQ(captured.finish(), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("suppressed_effects"), "(int) 2\n");
}

TEST(ExpressionValuesTest, SemicolonClassPrvaluesRunTheirDestructorOnce) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int n = 0; struct P { P() = default; "
                           "P(const P&) = delete; ~P() { ++n; } };"));

  StdoutCapture captured;
  ASSERT_TRUE(cpp->execute("P{};"));
  EXPECT_TRUE(captured.finish().empty());
  EXPECT_EQ(cpp->evaluate("n"), "(int) 1\n");

  ASSERT_TRUE(
      cpp->execute("struct Counted { Counted() = default; Counted(const Counted&) = delete; "
                   "~Counted() { ++n; } }; "
                   "Counted make_counted() { return Counted{}; } "
                   "struct Nested { Counted value; }; "
                   "Nested make_nested() { return Nested{}; }"));
  ASSERT_TRUE(cpp->execute("make_counted();"));
  EXPECT_EQ(cpp->evaluate("n"), "(int) 2\n");
  ASSERT_TRUE(cpp->execute("make_nested();"));
  EXPECT_EQ(cpp->evaluate("n"), "(int) 3\n");
}

TEST(ExpressionValuesTest, ScalarResultsPreserveTemporaryCleanups) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int scalar_cleanup_count = 0; "
                           "struct ScalarCleanup { "
                           "  ScalarCleanup() = default; "
                           "  ScalarCleanup(const ScalarCleanup&) = delete; "
                           "  ~ScalarCleanup() { ++scalar_cleanup_count; } "
                           "  int value() const { return 42; } "
                           "}; "
                           "int consume(const ScalarCleanup& value) { return value.value(); }"));

  StdoutCapture captured;
  ASSERT_TRUE(cpp->execute("ScalarCleanup{}.value();"));
  EXPECT_EQ(cpp->evaluate("scalar_cleanup_count"), "(int) 1\n");
  ASSERT_TRUE(cpp->execute("consume(ScalarCleanup{});"));
  EXPECT_EQ(cpp->evaluate("scalar_cleanup_count"), "(int) 2\n");
  EXPECT_TRUE(captured.finish().empty());
}

TEST(ExpressionValuesTest, ParseRecoveryDoesNotAttemptValueCapture) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int identity(int value) { return value; }"));

  EXPECT_FALSE(cpp->execute("unknown_value;"));
  EXPECT_FALSE(cpp->execute("identity(\"wrong type\");"));

  StdoutCapture captured;
  ASSERT_TRUE(cpp->execute("identity(4);"));
  EXPECT_EQ(captured.finish(), "(int) 4\n");
}

} // namespace
} // namespace example
