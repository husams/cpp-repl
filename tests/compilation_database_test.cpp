#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

namespace fs = std::filesystem;

class TemporaryProject {
 public:
  TemporaryProject() {
    static std::atomic<unsigned> next{0};
    root_ = fs::temp_directory_path() /
            ("cpp_repl_compdb_" + std::to_string(::getpid()) + "_" +
             std::to_string(next++));
    fs::create_directories(root_);
  }
  ~TemporaryProject() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  const fs::path& root() const { return root_; }
  fs::path write(const fs::path& relative, const std::string& contents) const {
    const fs::path file = root_ / relative;
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << contents;
    return file;
  }

 private:
  fs::path root_;
};

class CurrentDirectory {
 public:
  explicit CurrentDirectory(const fs::path& path) : old_(fs::current_path()) {
    fs::current_path(path);
  }
  ~CurrentDirectory() {
    std::error_code ec;
    fs::current_path(old_, ec);
  }

 private:
  fs::path old_;
};

std::unique_ptr<CppInterpreter> create_interpreter(std::string& error) {
  return CppInterpreter::create({}, error);
}

std::string quote_json(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string command_entry(const fs::path& directory, const fs::path& file,
                          const std::string& command) {
  return "{\"directory\":" + quote_json(directory.string()) +
         ",\"file\":" + quote_json(file.string()) +
         ",\"command\":" + quote_json(command) + "}";
}

std::string arguments_entry(const fs::path& directory, const fs::path& file,
                            const std::vector<std::string>& arguments) {
  std::string out = "{\"directory\":" + quote_json(directory.string()) +
                    ",\"file\":" + quote_json(file.string()) +
                    ",\"arguments\":[";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) out += ',';
    out += quote_json(arguments[i]);
  }
  return out + "]}";
}

void write_database(const TemporaryProject& project, const std::string& entries,
                    const fs::path& relative = "compile_commands.json") {
  project.write(relative, "[" + entries + "]\n");
}

TEST(CompilationDatabase,
     LoadsAncestorBuildDatabaseAndPerFileOptionsAtStartup) {
  TemporaryProject project;
  const fs::path build = project.root() / "build";
  const fs::path source_dir = project.root() / "src";
  fs::create_directories(build);
  project.write("include one/config.hpp",
                "#pragma once\n#define CONFIG_VALUE 41\n");
  project.write("include-two/config.hpp",
                "#pragma once\n#define CONFIG_VALUE 73\n");
  const fs::path first_source = project.write(
      "src/first.cpp",
      "#include <concepts>\n#include \"config.hpp\"\n"
      "#if __cplusplus < 202002L\n#error requires C++20\n#endif\n"
      "#if !defined(FIRST_ONLY) || defined(SECOND_ONLY)\n#error wrong per-file "
      "macro set\n#endif\n"
      "template<class T> concept FirstHasValue = requires(T t) { t.value(); "
      "};\n"
      "struct First { int value() const { return CONFIG_VALUE; } };\n"
      "template<FirstHasValue T> int first_value(T x) { return x.value(); }\n"
      "int from_first() { return first_value(First{}) + FIRST_ONLY; }\n");
  const fs::path second_source = project.write(
      "src/second.cpp",
      "#include \"config.hpp\"\n"
      "#if __cplusplus >= 202002L\n#error first source standard "
      "leaked\n#endif\n"
      "#if __cplusplus < 201703L\n#error second source requires C++17\n#endif\n"
      "#if !defined(SECOND_ONLY) || defined(FIRST_ONLY)\n#error wrong per-file "
      "macro set\n#endif\n"
      "struct Second { int value() const { return CONFIG_VALUE; } };\n"
      "int second_value(Second x) { return x.value(); }\n"
      "int from_second() { return second_value(Second{}) + SECOND_ONLY; }\n");
  const std::string first =
      command_entry(build, fs::path("../src/first.cpp"),
                    "clang++ -std=c++20 -DFIRST_ONLY=1 -I'../include one' -c "
                    "../src/first.cpp");
  const std::string second =
      arguments_entry(build, fs::path("../src/second.cpp"),
                      {"clang++", "-std=c++17", "-DSECOND_ONLY=1",
                       "-I../include-two", "-c", "../src/second.cpp"});
  write_database(project, first + "," + second, "build/compile_commands.json");

  // The database is found in an ancestor's conventional build directory and
  // its contents are captured when the interpreter is created.
  CurrentDirectory cwd(source_dir);
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int prior_session_state = 9;"));

  project.write("build/compile_commands.json", "this is no longer JSON");
  const CppInterpreter::AddResult added_first =
      cpp->add_source(first_source.string());
  ASSERT_TRUE(added_first.ok) << added_first.message;
  EXPECT_EQ(fs::current_path(), fs::canonical(source_dir));
  const CppInterpreter::AddResult added_second =
      cpp->add_source(second_source.string());
  ASSERT_TRUE(added_second.ok) << added_second.message;
  EXPECT_EQ(cpp->evaluate("from_first()"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("from_second()"), "(int) 74\n");
  EXPECT_EQ(cpp->evaluate("prior_session_state"), "(int) 9\n");
}

TEST(CompilationDatabase, AppliesCommandMacrosAndRelativeForcedIncludeFiles) {
  TemporaryProject project;
  const fs::path build = project.root() / "build";
  fs::create_directories(build);
  project.write(
      "build/relative_include/forced.hpp",
      "#pragma once\ninline constexpr int forced_include_value = 19;\n");
  project.write(
      "build/relative_include/macro_values.h",
      "#define IMACRO_VALUE 23\nstruct ImacrosMustNotDeclareTypes {};\n");
  project.write("build/relative_include/include_value.hpp",
                "#pragma once\n#define INCLUDE_VALUE 11\n");
  const fs::path configured = project.write(
      "src/options.cpp",
      "#include <include_value.hpp>\n"
      "#if !defined(DEFINED_FLAG) || defined(REMOVED_FLAG)\n"
      "#error -D/-U compile options were not applied\n#endif\n"
      "#if FUNC(2, 3) != 5\n#error function-like -D was not applied\n#endif\n"
      "int configured_options_value() { return DEFINED_FLAG + FUNC(2, 3) + "
      "INCLUDE_VALUE + forced_include_value + IMACRO_VALUE; }\n");
  project.write(
      "after/options_restored.cpp",
      "#if defined(DEFINED_FLAG) || defined(REMOVED_FLAG) || defined(FUNC) || "
      "defined(IMACRO_VALUE)\n#error source flags leaked into later "
      "input\n#endif\n"
      "#if __has_include(<include_value.hpp>)\n#error include search path "
      "leaked\n#endif\n"
      "int options_restored_value() { return 64; }\n");
  write_database(
      project,
      command_entry(
          build, "../src/options.cpp",
          "clang++ -std=c++20 -DDEFINED_FLAG=50 -DREMOVED_FLAG=1 "
          "-UREMOVED_FLAG -D'FUNC(x,y)=((x)+(y))' "
          "-Irelative_include -include forced.hpp -imacros macro_values.h "
          "-c ../src/options.cpp"),
      "build/compile_commands.json");

  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  const CppInterpreter::AddResult configured_result =
      cpp->add_source(configured.string());
  ASSERT_TRUE(configured_result.ok) << configured_result.message;
  EXPECT_EQ(cpp->evaluate("configured_options_value()"), "(int) 108\n");

  const fs::path restored = project.root() / "after/options_restored.cpp";
  const CppInterpreter::AddResult restored_result =
      cpp->add_source(restored.string());
  ASSERT_TRUE(restored_result.ok) << restored_result.message;
  EXPECT_EQ(cpp->evaluate("options_restored_value()"), "(int) 64\n");
}

TEST(CompilationDatabase,
     FailedAddRestoresOptionsAndPreservesSessionValuesAndHooks) {
  TemporaryProject project;
  const fs::path build = project.root() / "build";
  fs::create_directories(build);
  project.write("good_include/good_only.hpp",
                "#pragma once\n#define GOOD_HEADER_VALUE 6\n");
  project.write("bad_include/bad_only.hpp", "#pragma once\n");
  const fs::path good = project.write(
      "src/good.cpp",
      "#include <good_only.hpp>\n"
      "#ifndef GOOD_ONLY\n#error good entry options missing\n#endif\n"
      "int database_good_value() { return GOOD_ONLY + GOOD_HEADER_VALUE; }\n");
  const fs::path bad = project.write(
      "src/bad.cpp",
      "#ifndef BAD_ONLY\n#error failing entry options missing\n#endif\n"
      "#include <bad_only.hpp>\n#error intentional add failure\n");
  const fs::path after = project.write(
      "after/after_failure.cpp",
      "#if defined(GOOD_ONLY) || defined(BAD_ONLY)\n#error database macros "
      "leaked\n#endif\n"
      "#if __has_include(<good_only.hpp>) || __has_include(<bad_only.hpp>)\n"
      "#error database include directories leaked\n#endif\n"
      "#if __cplusplus >= 202002L\n#error database standard leaked\n#endif\n"
      "int after_failed_add() { return preserved_value; }\n");
  const std::string good_entry =
      arguments_entry(build, "../src/good.cpp",
                      {"clang++", "-std=c++20", "-DGOOD_ONLY=30",
                       "-I../good_include", "-c", "../src/good.cpp"});
  const std::string bad_entry =
      arguments_entry(build, "../src/bad.cpp",
                      {"clang++", "-std=c++20", "-DBAD_ONLY=1",
                       "-I../bad_include", "-c", "../src/bad.cpp"});
  const std::string after_entry = arguments_entry(
      build, "../after/after_failure.cpp",
      {"clang++", "-std=c++17", "-c", "../after/after_failure.cpp"});
  write_database(project, good_entry + "," + bad_entry + "," + after_entry,
                 "build/compile_commands.json");

  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int preserved_value = 12;"));
  ASSERT_TRUE(
      cpp->execute("int database_hook_target(int x) { return x + 1; }"));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "int database_hook_target(int x)",
                            "return x + 100;", error))
      << error;

  const CppInterpreter::AddResult good_result = cpp->add_source(good.string());
  ASSERT_TRUE(good_result.ok) << good_result.message;
  EXPECT_EQ(cpp->evaluate("database_good_value()"), "(int) 36\n");

  const CppInterpreter::AddResult bad_result = cpp->add_source(bad.string());
  EXPECT_FALSE(bad_result.ok);
  EXPECT_EQ(cpp->sources().size(), 1u);

  const CppInterpreter::AddResult after_result =
      cpp->add_source(after.string());
  ASSERT_TRUE(after_result.ok) << after_result.message;
  EXPECT_EQ(cpp->evaluate("after_failed_add()"), "(int) 12\n");
  EXPECT_EQ(cpp->evaluate("database_hook_target(3)"), "(int) 103\n");
  EXPECT_EQ(cpp->evaluate("database_good_value()"), "(int) 36\n");
}

TEST(CompilationDatabase,
     SameDirectoryEntryIsFallbackForHeaderAndOtherCppExtension) {
  TemporaryProject project;
  const fs::path build = project.root() / "build";
  fs::create_directories(build);
  project.write("src/reference.cpp", "// compile entry\n");
  const fs::path header =
      project.write("src/fallback.hpp",
                    "#if !defined(FALLBACK_VALUE)\n#error missing compile "
                    "database fallback\n#endif\n"
                    "inline int from_header() { return FALLBACK_VALUE; }\n");
  const fs::path other_extension =
      project.write("src/fallback.cc",
                    "#if !defined(FALLBACK_VALUE)\n#error missing compile "
                    "database fallback\n#endif\n"
                    "int from_cc_file() { return FALLBACK_VALUE + 1; }\n");
  write_database(
      project,
      arguments_entry(build, "../src/reference.cpp",
                      {"clang++", "-std=c++20", "-DFALLBACK_VALUE=56", "-c",
                       "../src/reference.cpp"}),
      "build/compile_commands.json");
  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  const CppInterpreter::AddResult header_result =
      cpp->add_source(header.string());
  ASSERT_TRUE(header_result.ok) << header_result.message;
  const CppInterpreter::AddResult cc_result =
      cpp->add_source(other_extension.string());
  ASSERT_TRUE(cc_result.ok) << cc_result.message;
  EXPECT_EQ(cpp->evaluate("from_header()"), "(int) 56\n");
  EXPECT_EQ(cpp->evaluate("from_cc_file()"), "(int) 57\n");
}

TEST(CompilationDatabase, DoesNotBorrowAnEntryFromAnotherDirectory) {
  TemporaryProject project;
  const fs::path build = project.root() / "build";
  fs::create_directories(build);
  project.write("src/known.cpp", "// compile entry\n");
  const fs::path unknown = project.write(
      "other/unknown.cc",
      "#if !defined(ONLY_KNOWN_DIRECTORY)\n#error no same-directory compile "
      "entry\n#endif\n"
      "int from_unknown_directory() { return ONLY_KNOWN_DIRECTORY; }\n");
  write_database(
      project,
      arguments_entry(build, "../src/known.cpp",
                      {"clang++", "-std=c++20", "-DONLY_KNOWN_DIRECTORY=88",
                       "-c", "../src/known.cpp"}),
      "build/compile_commands.json");

  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  const CppInterpreter::AddResult result = cpp->add_source(unknown.string());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(cpp->sources().empty());
  EXPECT_EQ(cpp->evaluate("6 * 7"), "(int) 42\n");
}

TEST(CompilationDatabase, MalformedDatabaseFailsInterpreterStartup) {
  TemporaryProject project;
  project.write("compile_commands.json", "[{ definitely not json ]");
  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  EXPECT_EQ(cpp, nullptr);
  EXPECT_FALSE(error.empty());
}

TEST(CompilationDatabase,
     NoDatabasePreservesDefaultsAndMissingSourceStillErrors) {
  TemporaryProject project;
  const fs::path source =
      project.write("plain.cpp", "int plain_file_value() { return 42; }\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  std::unique_ptr<CppInterpreter> cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  EXPECT_EQ(cpp->evaluate("2 + 3"), "(int) 5\n");
  const CppInterpreter::AddResult added = cpp->add_source(source.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("plain_file_value()"), "(int) 42\n");
  EXPECT_EQ(cpp->add_source((project.root() / "missing.cpp").string()).message,
            "no such file: " + (project.root() / "missing.cpp").string());
}

TEST(CompilationDatabase,
     ExpandsResponseFilesAtStartupStripsBuildOutputsAndHonorsCliOverrides) {
  TemporaryProject project;
  const auto build = project.root() / "build";
  fs::create_directories(build);
  const auto source =
      project.write("src/response.cxx",
                    "#if __cplusplus < 202002L\n#error requires C++20\n#endif\n"
                    "int response_value() { return RESPONSE_VALUE; }\n");
  project.write("build/options.rsp",
                "-std=c++20 -DRESPONSE_VALUE=17 -c ../src/response.cxx -o "
                "emitted.o -MMD -MF emitted.d");
  write_database(project,
                 arguments_entry(build, "../src/response.cxx",
                                 {"clang++", "@options.rsp"}),
                 "build/compile_commands.json");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = CppInterpreter::create({"-DRESPONSE_VALUE=42"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  project.write("build/options.rsp", "no longer a valid compiler response");
  auto added = cpp->add_source(source.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("response_value()"), "(int) 42\n");
  EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));
  EXPECT_FALSE(fs::exists(build / "emitted.o"));
  EXPECT_FALSE(fs::exists(build / "emitted.d"));
}

TEST(CompilationDatabase,
     MissingForcedHeadersAndInvalidMacroHeadersLeaveSessionUsable) {
  for (const auto& option : {"-include", "-imacros"}) {
    for (const bool missing : {true, false}) {
      TemporaryProject project;
      const auto build = project.root() / "build";
      fs::create_directories(build);
      const auto source =
          project.write("source.cpp", "int never_added() { return 0; }\n");
      if (!missing)
        project.write("build/options.hpp",
                      "#error intentional compile-header failure\n");
      write_database(
          project,
          arguments_entry(build, "../source.cpp",
                          {"clang++", "-std=c++20", "-DSCOPED_FLAG=1", option,
                           "options.hpp", "-c", "../source.cpp"}));
      CurrentDirectory cwd(project.root());
      std::string error;
      auto cpp = create_interpreter(error);
      ASSERT_NE(cpp, nullptr) << error;
      ASSERT_TRUE(cpp->execute("int kept_after_header_failure = 42;"));
      EXPECT_FALSE(cpp->add_source(source.string()).ok);
      EXPECT_TRUE(cpp->sources().empty());
      EXPECT_EQ(cpp->evaluate("kept_after_header_failure"), "(int) 42\n");
      EXPECT_TRUE(cpp->execute(
          "#ifdef SCOPED_FLAG\n#error compiler flag leaked\n#endif\n"));
      EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));
    }
  }
}

}  // namespace
}  // namespace example
