#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "cpp_interpreter.hpp"

namespace example {
namespace {
class IterativeDevelopment : public ::testing::Test {
protected:
  void SetUp() override {
    std::string error;
    cpp = CppInterpreter::create({}, error);
    ASSERT_TRUE(cpp) << error;
  }
  std::unique_ptr<CppInterpreter> cpp;
};

TEST_F(IterativeDevelopment, RedefinesClassAndPreservesExistingObjects) {
  ASSERT_TRUE(cpp->execute("class A { public: int value() { return 1; } };"));
  ASSERT_TRUE(cpp->execute("A old_a; int old_size = sizeof(A);"));
  ASSERT_TRUE(cpp->execute(
      "class A { public: double extra = 3; int value() { return 2; } };"));
  ASSERT_TRUE(cpp->execute("A new_a;"));
  EXPECT_EQ(cpp->evaluate("old_a.value()"), "(int) 1\n");
  EXPECT_EQ(cpp->evaluate("new_a.value()"), "(int) 2\n");
  EXPECT_EQ(cpp->evaluate("sizeof(A) > old_size"), "(bool) true\n");
  EXPECT_EQ(cpp->class_names(), (std::vector<std::string>{"A"}));
}

TEST_F(IterativeDevelopment, RedefinesFunctionsAndKeepsOldCallersAndOverloads) {
  ASSERT_TRUE(cpp->execute("int f(int x) { return x + 1; }"));
  ASSERT_TRUE(cpp->execute("double f(double x) { return x + 0.5; }"));
  ASSERT_TRUE(cpp->execute("int old_caller() { return f(1); }"));
  ASSERT_TRUE(cpp->execute("int f(int x) { return x + 10; }"));
  EXPECT_EQ(cpp->evaluate("f(1)"), "(int) 11\n");
  EXPECT_EQ(cpp->evaluate("f(1.0)"), "(double) 1.5000000\n");
  EXPECT_EQ(cpp->evaluate("old_caller()"), "(int) 2\n");
}

TEST_F(IterativeDevelopment, RedefinesVariablesAliasesAndNamespacedClasses) {
  ASSERT_TRUE(cpp->execute("int value = 1; using Number = int;"));
  ASSERT_TRUE(cpp->execute("double value = 2.5; using Number = double;"));
  EXPECT_EQ(cpp->evaluate("value"), "(double) 2.5000000\n");
  ASSERT_TRUE(cpp->execute("Number number = 3.5;"));
  EXPECT_EQ(cpp->evaluate("number"), "(double) 3.5000000\n");
  ASSERT_TRUE(cpp->execute("namespace app { struct A { int value = 1; }; }"));
  ASSERT_TRUE(cpp->execute("app::A old;"));
  ASSERT_TRUE(cpp->execute("namespace app { struct A { int value = 2; }; }"));
  ASSERT_TRUE(cpp->execute("app::A current;"));
  EXPECT_EQ(cpp->evaluate("old.value"), "(int) 1\n");
  EXPECT_EQ(cpp->evaluate("current.value"), "(int) 2\n");
}

TEST_F(IterativeDevelopment, RejectedInputRestoresPreviousDefinitions) {
  ASSERT_TRUE(cpp->execute("int value = 7;"));
  EXPECT_FALSE(cpp->execute("int value = 9; this is invalid;"));
  EXPECT_EQ(cpp->evaluate("value"), "(int) 7\n");
  ASSERT_TRUE(cpp->execute("int missing();"));
  EXPECT_FALSE(cpp->execute("int value = missing();"));
  EXPECT_EQ(cpp->evaluate("value"), "(int) 7\n");
  ASSERT_TRUE(cpp->execute("int value = 11;"));
  EXPECT_EQ(cpp->evaluate("value"), "(int) 11\n");
}

TEST_F(IterativeDevelopment,
       RedefinesTemplatesEnumsAndPreviouslyDeclaredFunctions) {
  ASSERT_TRUE(cpp->execute("template<class T> T twice(T x) { return x * 2; }"));
  EXPECT_EQ(cpp->evaluate("twice(3)"), "(int) 6\n");
  ASSERT_TRUE(cpp->execute("template<class T> T twice(T x) { return x * 3; }"));
  EXPECT_EQ(cpp->evaluate("twice(3)"), "(int) 9\n");
  ASSERT_TRUE(cpp->execute("enum E { One = 1 };"));
  ASSERT_TRUE(cpp->execute("enum E { One = 2 };"));
  EXPECT_EQ(cpp->evaluate("int(One)"), "(int) 2\n");
  ASSERT_TRUE(cpp->execute("int declared();"));
  ASSERT_TRUE(cpp->execute("int declared() { return 1; }"));
  ASSERT_TRUE(cpp->execute("int declared() { return 2; }"));
  EXPECT_EQ(cpp->evaluate("declared()"), "(int) 2\n");
}

class SourceFiles {
public:
  SourceFiles() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "cpp-repl-reload-XXXXXX")
            .string();
    if (auto *created = mkdtemp(pattern.data()))
      directory = std::filesystem::canonical(created);
  }
  ~SourceFiles() {
    std::error_code error;
    if (!directory.empty())
      std::filesystem::remove_all(directory, error);
  }
  std::string write(const std::string &name, const std::string &code) {
    const auto path = directory / name;
    std::ofstream(path) << code;
    return path.string();
  }
  std::filesystem::path directory;
};

TEST_F(IterativeDevelopment, ReloadsChangedCppAndRetainsOldObjects) {
  SourceFiles files;
  ASSERT_FALSE(files.directory.empty());
  const auto path = files.write(
      "source.cpp",
      "struct A { int value() { return 1; } }; int answer() { return 10; }");
  ASSERT_TRUE(cpp->add_source(path).ok);
  ASSERT_TRUE(cpp->execute("A old; int unchanged = 42;"));
  files.write(
      "source.cpp",
      "struct A { int value() { return 2; } }; int answer() { return 20; }");
  auto result = cpp->reload_source(path);
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 20\n");
  ASSERT_TRUE(cpp->execute("A current;"));
  EXPECT_EQ(cpp->evaluate("current.value()"), "(int) 2\n");
  EXPECT_EQ(cpp->evaluate("old.value()"), "(int) 1\n");
  EXPECT_EQ(cpp->evaluate("unchanged"), "(int) 42\n");
  EXPECT_EQ(cpp->sources().size(), 1u);
  std::string error;
  auto info = cpp->symbol_info("answer", error);
  ASSERT_TRUE(info) << error;
  EXPECT_EQ(info->find("2 matches"), std::string::npos) << *info;
}

TEST_F(IterativeDevelopment, AutomaticallyReloadsBeforeTheNextCppInput) {
  SourceFiles files;
  const auto path = files.write("source.cpp", "int answer() { return 1; }");
  ASSERT_TRUE(cpp->add_source(path).ok);
  files.write("source.cpp", "int answer() { return 2; }");
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
  files.write("source.cpp", "int answer() { return 3; }");
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 3\n");
}

TEST_F(IterativeDevelopment, ReloadsGuardedHeadersAndRecoversFromFailure) {
  SourceFiles files;
  files.write(
      "answer.hpp",
      "#pragma once\n#define VALUE 1\nstruct A { int value = VALUE; };\n");
  const auto path = files.write(
      "source.cpp",
      "#include \"answer.hpp\"\nint answer() { return A{}.value; }");
  ASSERT_TRUE(cpp->add_source(path).ok);
  files.write(
      "answer.hpp",
      "#pragma once\n#define VALUE 2\nstruct A { int value = VALUE; };\n");
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
  files.write("answer.hpp",
              "#ifndef ANSWER_HPP\n#define ANSWER_HPP\n#define VALUE 9\nstruct "
              "A { this is invalid; };\n#endif\n");
  EXPECT_FALSE(cpp->reload_source(path).ok);
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
  EXPECT_EQ(cpp->evaluate("VALUE"), "(int) 2\n");
  files.write("answer.hpp",
              "#ifndef ANSWER_HPP\n#define ANSWER_HPP\n#define VALUE 3\nstruct "
              "A { int value = VALUE; };\n#endif\n");
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 3\n");
}

TEST_F(IterativeDevelopment, ReloadsPrototypesAndOutOfLineMethods) {
  SourceFiles files;
  files.write("answer.hpp",
              "#pragma once\nstruct A { int value(); };\nint answer();\n");
  const auto path = files.write(
      "source.cpp", "#include \"answer.hpp\"\nint answer() { return 1; }\nint "
                    "A::value() { return answer(); }");
  ASSERT_TRUE(cpp->add_source(path).ok);
  ASSERT_TRUE(cpp->execute("A old;"));
  files.write("source.cpp", "#include \"answer.hpp\"\nint answer() { return 2; "
                            "}\nint A::value() { return answer(); }");
  auto result = cpp->reload_source(path);
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
  ASSERT_TRUE(cpp->execute("A current;"));
  EXPECT_EQ(cpp->evaluate("old.value()"), "(int) 1\n");
  EXPECT_EQ(cpp->evaluate("current.value()"), "(int) 2\n");
}

TEST_F(IterativeDevelopment, FailedFirstAddRestoresMacrosAndCanBeRetried) {
  SourceFiles files;
  ASSERT_TRUE(cpp->execute("#define VALUE 1\n"));
  const auto path = files.write(
      "source.cpp", "#pragma once\n#define VALUE 9\nint broken = ;\n");
  EXPECT_FALSE(cpp->add_source(path).ok);
  EXPECT_EQ(cpp->evaluate("VALUE"), "(int) 1\n");
  files.write(
      "source.cpp",
      "#pragma once\n#define VALUE 2\nint answer() { return VALUE; }\n");
  const auto result = cpp->add_source(path);
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
}

TEST_F(IterativeDevelopment, FailedAddRestoresIncludeOnceHeaders) {
  SourceFiles files;
  const auto header =
      files.write("answer.hpp", "#pragma once\nstruct A { int value = 4; };\n");
  const auto path =
      files.write("source.cpp", "#include \"answer.hpp\"\nint broken = ;\n");
  EXPECT_FALSE(cpp->add_source(path).ok);
  ASSERT_TRUE(cpp->execute("#include \"" + header + "\"\n"));
  EXPECT_EQ(cpp->evaluate("A{}.value"), "(int) 4\n");
}

TEST_F(IterativeDevelopment,
       ReloadRemovesDeletedNamesAndRecoversFromMissingSymbols) {
  SourceFiles files;
  const auto path = files.write(
      "source.cpp", "int removed() { return 9; } int answer() { return 1; }");
  ASSERT_TRUE(cpp->add_source(path).ok);
  files.write("source.cpp", "int answer() { return 2; }");
  ASSERT_TRUE(cpp->reload_source(path).ok);
  EXPECT_FALSE(cpp->evaluate("removed()"));
  files.write("source.cpp",
              "int missing(); int answer() { return missing(); }");
  EXPECT_FALSE(cpp->reload_source(path).ok);
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 2\n");
  files.write("source.cpp", "int answer() { return 3; }");
  EXPECT_EQ(cpp->evaluate("answer()"), "(int) 3\n");
}
} // namespace
} // namespace example
