#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

bool has(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

struct IncludeTree {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("cpp-repl-editor-context-" + std::to_string(getpid()));
  std::filesystem::path quoted = root / "quoted";
  std::filesystem::path system = root / "system";

  IncludeTree() {
    std::filesystem::create_directories(quoted / "nested");
    std::filesystem::create_directories(system / "nested");
    std::ofstream(quoted / "alpha_header.hpp") << "#pragma once\n";
    std::ofstream(quoted / "__detail.hpp") << "#pragma once\n";
    std::ofstream(quoted / "nested" / "beta_header.hpp") << "#pragma once\n";
    std::ofstream(system / "system_header.hpp") << "#pragma once\n";
    std::ofstream(system / "nested" / "system_nested.hpp") << "#pragma once\n";
  }

  ~IncludeTree() { std::filesystem::remove_all(root); }
};

TEST(EditorContextTest, IncludePathCompletionReportsReplacementPrefix) {
  IncludeTree tree;
  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.quoted.string(), "-isystem", tree.system.string()},
                                    error);
  ASSERT_NE(cpp, nullptr) << error;

  const auto quoted = cpp->complete("#include \"alpha_he");
  EXPECT_TRUE(has(quoted.names, "alpha_header.hpp"));
  EXPECT_EQ(quoted.prefix, "alpha_he");
  EXPECT_TRUE(has(cpp->complete("#include \"__d").names, "__detail.hpp"));

  const auto extension = cpp->complete("#include \"alpha_header.h");
  EXPECT_TRUE(has(extension.names, "alpha_header.hpp"));
  EXPECT_EQ(extension.prefix, "alpha_header.h");

  const auto directory = cpp->complete("#include \"ne");
  EXPECT_TRUE(has(directory.names, "nested/"));
  EXPECT_EQ(directory.prefix, "ne");

  const auto nested = cpp->complete("#include \"nested/beta_he");
  EXPECT_TRUE(has(nested.names, "nested/beta_header.hpp"));
  EXPECT_EQ(nested.prefix, "nested/beta_he");

  const auto nested_directory = cpp->complete("#include \"nested/");
  EXPECT_TRUE(has(nested_directory.names, "nested/beta_header.hpp"));
  EXPECT_EQ(nested_directory.prefix, "nested/");

  const auto system = cpp->complete("#include <system_he");
  EXPECT_TRUE(has(system.names, "system_header.hpp"));
  EXPECT_EQ(system.prefix, "system_he");

  const auto system_nested = cpp->complete("#include <nested/system_ne");
  EXPECT_TRUE(has(system_nested.names, "nested/system_nested.hpp"));
  EXPECT_EQ(system_nested.prefix, "nested/system_ne");
}

TEST(EditorContextTest, MemberCompletionRespectsTypeAndAccessContext) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "struct EditorBase { int inherited_field; }; "
      "struct EditorObject : EditorBase { int field; int method() const; int mutable_method(); "
      "private: int hidden_field; }; "
      "EditorObject editor_object; EditorObject* editor_pointer; "
      "int EditorObjectValue; const EditorObject editor_const{}; "
      "EditorObject editor_factory() { return {}; }"));

  EXPECT_TRUE(has(cpp->complete("editor_object.fi").names, "field"));
  EXPECT_TRUE(has(cpp->complete("editor_object.inherited_").names, "inherited_field"));
  EXPECT_TRUE(has(cpp->complete("editor_pointer->fi").names, "field"));
  EXPECT_TRUE(has(cpp->complete("editor_factory().me").names, "method"));
  EXPECT_TRUE(has(cpp->complete("editor_const.me").names, "method"));
  EXPECT_FALSE(has(cpp->complete("editor_const.mutable_").names, "mutable_method"));
  EXPECT_FALSE(has(cpp->complete("editor_object.hidden_").names, "hidden_field"));
  const auto type_context = cpp->complete("using EditorAlias = EditorO");
  EXPECT_TRUE(has(type_context.names, "EditorObject"));
  EXPECT_FALSE(has(type_context.names, "EditorObjectValue"));
  EXPECT_FALSE(has(cpp->complete("// editor_").names, "editor_object"));
  const auto function = cpp->complete("editor_fac");
  const auto factory = std::find_if(function.items.begin(), function.items.end(),
                                    [](const auto& i) { return i.text == "editor_factory"; });
  ASSERT_NE(factory, function.items.end());
  EXPECT_NE(factory->label.find("editor_factory()"), std::string::npos);
}

TEST(EditorContextTest, IncompleteContextCompletionReturnsWithoutSessionMutation) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::string error;
    auto cpp = CppInterpreter::create({}, error);
    if (!cpp) {
      _exit(2);
    }
    for (const auto* partial :
         {"struct EditorPartial { int value =", "struct EditorPartial { int value = 1; int next =",
          "void editor_partial() { int local =", "int editor_partial_expression() { return 1 +"}) {
      cpp->complete(partial);
    }
    _exit(cpp->evaluate("20 + 22") == std::optional<std::string>("(int) 42\n") ? 0 : 3);
  }
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  pid_t waited = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    waited = waitpid(child, &status, WNOHANG);
    if (waited != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (waited != child) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    FAIL() << "editor completion exceeded its 10 second deadline";
  }
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
} // namespace example
