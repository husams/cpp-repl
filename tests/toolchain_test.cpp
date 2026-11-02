#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "cpp_interpreter.hpp"
#include "project_compilation_database.hpp"

namespace example {
namespace {

class ToolchainConfiguration : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> next{0};
    root = std::filesystem::temp_directory_path() /
        ("cpp-repl-toolchain-" + std::to_string(getpid()) + "-" + std::to_string(next++));
    std::filesystem::create_directories(root / "custom includes");
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path root;
};

TEST_F(ToolchainConfiguration, ExplicitConfigAndIncludesSurviveCompilerScopes) {
  const auto config = root / "custom driver.cfg";
  {
    std::ofstream out(root / "custom includes" / "toolchain_marker.h");
    out << "#define TOOLCHAIN_HEADER_VALUE 17\n";
  }
  {
    std::ofstream out(config);
    out << "-std=c++17\n-DREPL_TOOLCHAIN_VALUE=1\n-isystem "
        << std::quoted((root / "custom includes").string()) << '\n';
  }
  std::string error;
  auto cpp = CppInterpreter::create(
      {"--config=" + config.string(), "-DREPL_TOOLCHAIN_VALUE=25", "-std=c++20"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <toolchain_marker.h>"));
  EXPECT_EQ(cpp->evaluate("REPL_TOOLCHAIN_VALUE + TOOLCHAIN_HEADER_VALUE"), "(int) 42\n");
  ASSERT_TRUE(cpp->execute("static_assert(__cplusplus >= 202002L);"));
  ASSERT_TRUE(cpp->add_compile_options({"-DSCOPE_MARKER=1"}, error)) << error;
  ASSERT_TRUE(cpp->execute("#include <toolchain_marker.h>\nstatic_assert(SCOPE_MARKER == 1);"));
  EXPECT_EQ(cpp->evaluate("REPL_TOOLCHAIN_VALUE + TOOLCHAIN_HEADER_VALUE"), "(int) 42\n");
}

TEST_F(ToolchainConfiguration, ExplicitResourceDirectoryOverridesBuildDefault) {
  const auto resources = root / "resources";
  std::filesystem::create_directories(resources / "include");
  {
    std::ofstream out(resources / "include" / "toolchain_builtin.h");
    out << "#define TOOLCHAIN_BUILTIN 42\n";
  }
  std::string error;
  auto cpp = CppInterpreter::create({"-resource-dir", resources.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <toolchain_builtin.h>"));
  EXPECT_EQ(cpp->evaluate("TOOLCHAIN_BUILTIN"), "(int) 42\n");
}

TEST_F(ToolchainConfiguration, ExplicitCxxHeadersOverridePlatformDefaults) {
  {
    std::ofstream out(root / "custom includes" / "vector");
    out << "namespace custom_std { inline int value() { return 42; } }\n";
  }
  std::string error;
  auto cpp = CppInterpreter::create(
      {"-nostdinc++", "-isystem", (root / "custom includes").string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <vector>"));
  EXPECT_EQ(cpp->evaluate("custom_std::value()"), "(int) 42\n");
}

TEST_F(ToolchainConfiguration, MissingExplicitConfigFailsAtStartup) {
  std::string error;
  auto cpp = CppInterpreter::create({"--config=" + (root / "missing.cfg").string()}, error);
  EXPECT_EQ(cpp, nullptr);
  EXPECT_FALSE(error.empty());
}

TEST_F(ToolchainConfiguration, RelativeDriverPathsResolveFromCommandDirectory) {
  {
    std::ofstream out(root / "driver.cfg");
    out << "-DRELATIVE_CONFIG=1\n";
  }
  std::string error;
  const auto flags = normalize_compile_flags(
      {"--config=./driver.cfg", "--gcc-toolchain=vendor gcc",
       "--gcc-install-dir=vendor gcc/lib/gcc/8.5.0"}, root.string(), error);
  ASSERT_TRUE(flags) << error;
  EXPECT_EQ(*flags, (std::vector<std::string>{
      "--config=" + (root / "driver.cfg").string(),
      "--gcc-toolchain=" + (root / "vendor gcc").string(),
      "--gcc-install-dir=" + (root / "vendor gcc/lib/gcc/8.5.0").string()}));
}

TEST_F(ToolchainConfiguration, StandardLibraryAndFilesystemMatchHost) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n#include <vector>\n#include <filesystem>"));
#ifdef __GLIBCXX__
  ASSERT_TRUE(cpp->execute("static_assert(__GLIBCXX__ == " + std::to_string(__GLIBCXX__) + ");"));
#endif
#ifdef _LIBCPP_VERSION
  ASSERT_TRUE(cpp->execute("static_assert(_LIBCPP_VERSION == " + std::to_string(_LIBCPP_VERSION) + ");"));
#endif
  ASSERT_TRUE(cpp->execute("std::string toolchain_text = \"hello\"; std::vector<int> toolchain_values{1, 2};"));
  EXPECT_EQ(cpp->evaluate("int(toolchain_text.size() + toolchain_values.size())"),
            "(int) 7\n");
  std::ostringstream path;
  path << "std::filesystem::path toolchain_root(" << std::quoted(root.string()) << ");";
  ASSERT_TRUE(cpp->execute(path.str()));
  EXPECT_EQ(cpp->evaluate("std::filesystem::exists(toolchain_root)"), "(bool) true\n");
  {
    std::ofstream out(root / "size.txt");
    out << "abc";
  }
  std::ostringstream file;
  file << "std::filesystem::path toolchain_file(" << std::quoted((root / "size.txt").string()) << ");";
  ASSERT_TRUE(cpp->execute(file.str()));
  EXPECT_EQ(cpp->evaluate("int(std::filesystem::file_size(toolchain_file))"),
            "(int) 3\n");
}

TEST_F(ToolchainConfiguration, RejectedInputPreservesBuiltinAllocationOverloads) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  EXPECT_FALSE(cpp->execute("#include <new>\nint toolchain_missing(); int rejected_allocation = toolchain_missing();"));
  ASSERT_TRUE(cpp->execute("int allocate_after_rejection() { void* p = __builtin_operator_new(32); __builtin_operator_delete(p); return 42; }"));
  EXPECT_EQ(cpp->evaluate("allocate_after_rejection()"), "(int) 42\n");
}

TEST_F(ToolchainConfiguration, ExplicitStandardLibraryRetainsPlatformDiscovery) {
  std::string error;
#ifdef _LIBCPP_VERSION
  const auto library = "-stdlib=libc++";
#else
  const auto library = "-stdlib=libstdc++";
#endif
  auto cpp = CppInterpreter::create({library}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>"));
  ASSERT_TRUE(cpp->execute("std::string selected_library_value = \"hello\";"));
  EXPECT_EQ(cpp->evaluate("int(selected_library_value.size())"), "(int) 5\n");
#ifdef _LIBCPP_VERSION
  // Disabling driver config files must not disable platform SDK/header defaults.
  auto no_config = CppInterpreter::create({library, "--no-default-config"}, error);
  ASSERT_NE(no_config, nullptr) << error;
  EXPECT_TRUE(no_config->execute("#include <string>\nstatic_assert(sizeof(std::string) > 0);"));
#endif
}

}  // namespace
}  // namespace example
