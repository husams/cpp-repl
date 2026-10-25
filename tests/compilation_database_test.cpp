#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cpp_interpreter.hpp"
#include "library_manager.hpp"
#include "startup_files.hpp"

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

TEST(CompilationDatabase, CompileScopesPreserveClangKeywordCompatibility) {
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->add_compile_options({"-DCOMPILE_SCOPE_CHECK=1"}, error))
      << error;
  // Older libstdc++ headers use builtin-trait spellings as member names.
  // Clang downgrades those keywords while parsing their declarations.
  ASSERT_TRUE(cpp->execute("#include <string>\n#include <vector>"));
  ASSERT_TRUE(cpp->execute("#include <iostream>"));
  EXPECT_EQ(cpp->evaluate("int(std::string(\"abc\").size())"), "(int) 3\n");
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
      "#include \"config.hpp\"\n"
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

TEST(CompileOptions,
     GlobalAppendReplaceClearAffectFutureInputsAndPreserveStateAndHooks) {
  TemporaryProject project;
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = CppInterpreter::create({"-DSTARTUP_VALUE=7"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "int existing_state = 12; int opt_target(int x) { return x; }"));
  ASSERT_TRUE(
      cpp->add_compile_options({"-O2", "-std=c++20", "-DOPT_VALUE=42"}, error))
      << error;
  EXPECT_EQ(cpp->evaluate("STARTUP_VALUE + OPT_VALUE"), "(int) 49\n");
  ASSERT_TRUE(cpp->execute(
      "template<class T> concept OptConcept = requires(T t) { t + 1; };"));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "opt_target(int x)",
                            "return x + OPT_VALUE;", error))
      << error;
  EXPECT_EQ(cpp->evaluate("opt_target(1)"), "(int) 43\n");
  ASSERT_TRUE(cpp->set_compile_options({"-std=c++17", "-DOPT_VALUE=9"}, error))
      << error;
  EXPECT_EQ(cpp->evaluate("OPT_VALUE"), "(int) 9\n");
  EXPECT_TRUE(
      cpp->execute("#ifdef STARTUP_VALUE\n#error startup flags were not "
                   "replaced\n#endif\n"));
  EXPECT_TRUE(
      cpp->execute("#if __cplusplus >= 202002L\n#error standard was not "
                   "replaced\n#endif\n"));
  EXPECT_EQ(cpp->evaluate("existing_state"), "(int) 12\n");
  EXPECT_EQ(cpp->evaluate("opt_target(1)"), "(int) 43\n");
  ASSERT_TRUE(cpp->set_compile_options({}, error)) << error;
  EXPECT_TRUE(
      cpp->execute("#if defined(OPT_VALUE) || defined(STARTUP_VALUE)\n#error "
                   "flags were not cleared\n#endif\n"));
  EXPECT_EQ(cpp->evaluate("existing_state"), "(int) 12\n");
}

TEST(CompileOptions,
     UseFileOptionsAsSessionReferencePreservesPathsStateAndHooks) {
  TemporaryProject project;
  const auto build = project.root() / "build";
  fs::create_directories(build);
  project.write(
      "include dir/reference.hpp",
      "#pragma once\ninline int reference_header_value() { return 30; }\n");
  const auto reference = project.write(
      "src/reference.cpp", "#error reference source must not be compiled\n");
  const auto other =
      project.write("other/another.cc",
                    "#include <reference.hpp>\nint adopted_options_value() { "
                    "return reference_header_value() + REFERENCE_VALUE; }\n");
  write_database(
      project,
      arguments_entry(build, "../src/reference.cpp",
                      {"clang++", "-std=c++20", "-DREFERENCE_VALUE=1",
                       "-I../include dir", "-c", "../src/reference.cpp"}),
      "build/compile_commands.json");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "int reference_state = 5; int reference_target(int x) { return x; }"));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "reference_target(int x)",
                            "return x + 10;", error))
      << error;
  ASSERT_TRUE(cpp->add_compile_options({"-DGLOBAL_VALUE=7"}, error)) << error;
  ASSERT_TRUE(cpp->add_compile_options({"-DREFERENCE_VALUE=12"}, error,
                                       reference.string()))
      << error;
  ASSERT_TRUE(cpp->use_compile_options("src/reference.cpp", error)) << error;
  EXPECT_TRUE(cpp->sources().empty());
  EXPECT_EQ(cpp->evaluate("REFERENCE_VALUE + GLOBAL_VALUE + reference_state"),
            "(int) 24\n");
  ASSERT_TRUE(
      cpp->execute("#include <reference.hpp>\n#if __cplusplus < "
                   "202002L\n#error missing reference standard\n#endif\n"));
  EXPECT_EQ(cpp->evaluate("reference_header_value()"), "(int) 30\n");
  EXPECT_EQ(cpp->evaluate("reference_target(2)"), "(int) 12\n");
  const auto added = cpp->add_source(other.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("adopted_options_value()"), "(int) 42\n");
  const auto info = cpp->describe_compile_options(error);
  ASSERT_TRUE(info) << error;
  EXPECT_NE(info->find("Reference file"), std::string::npos);
  EXPECT_NE(info->find(fs::canonical(reference).string()), std::string::npos);
  EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));

  // Adoption is a snapshot; later file changes require selecting it again.
  ASSERT_TRUE(cpp->set_compile_options({"-DREFERENCE_VALUE=20"}, error,
                                       reference.string()))
      << error;
  EXPECT_EQ(cpp->evaluate("REFERENCE_VALUE"), "(int) 12\n");
  ASSERT_TRUE(cpp->use_compile_options(reference.string(), error)) << error;
  EXPECT_EQ(cpp->evaluate("REFERENCE_VALUE"), "(int) 20\n");
  ASSERT_TRUE(cpp->set_compile_options({}, error)) << error;
  EXPECT_TRUE(
      cpp->execute("#ifdef REFERENCE_VALUE\n#error reference options were not "
                   "cleared\n#endif\n"));
  const auto cleared = cpp->describe_compile_options(error);
  ASSERT_TRUE(cleared) << error;
  EXPECT_EQ(cleared->find("Reference file"), std::string::npos);
}

TEST(CompileOptions, FailedReferenceSelectionKeepsExistingSessionOptions) {
  TemporaryProject project;
  const auto good = project.write("src/good.cpp", "// reference only\n");
  project.write("bad.cpp", "// invalid command\n");
  write_database(project,
                 arguments_entry(
                     project.root(), "bad.cpp",
                     {"clang++", "-fnot-a-reference-option", "-c", "bad.cpp"}));
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(
      cpp->set_compile_options({"-DREFERENCE_OK=42"}, error, good.string()))
      << error;
  ASSERT_TRUE(cpp->use_compile_options(good.string(), error)) << error;
  const auto before = cpp->describe_compile_options(error);
  ASSERT_TRUE(before) << error;
  for (const auto& file : {"", "missing/file.cpp", "bad.cpp"}) {
    EXPECT_FALSE(cpp->use_compile_options(file, error)) << file;
    EXPECT_FALSE(error.empty()) << file;
    EXPECT_EQ(cpp->describe_compile_options(error), before);
    EXPECT_EQ(cpp->evaluate("REFERENCE_OK"), "(int) 42\n");
  }
}

TEST(CompileOptions,
     SwitchingReferenceRebasesFlagsAndKeepsManualGlobalOverrides) {
  TemporaryProject project;
  project.write("a.cpp", "// reference A\n");
  project.write("b.cpp", "// reference B\n");
  write_database(
      project, arguments_entry(
                   project.root(), "a.cpp",
                   {"clang++", "-std=c++20", "-DREF_VALUE=1", "-c", "a.cpp"}) +
                   "," +
                   arguments_entry(project.root(), "b.cpp",
                                   {"clang++", "-std=c++17", "-DREF_VALUE=2",
                                    "-c", "b.cpp"}));
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->add_compile_options({"-DKEEP_VALUE=5"}, error)) << error;
  ASSERT_TRUE(cpp->use_compile_options("a.cpp", error)) << error;
  EXPECT_EQ(cpp->evaluate("REF_VALUE"), "(int) 1\n");
  ASSERT_TRUE(cpp->add_compile_options({"-DNEW_VALUE=6"}, error)) << error;
  ASSERT_TRUE(cpp->use_compile_options("b.cpp", error)) << error;
  EXPECT_EQ(cpp->evaluate("REF_VALUE + KEEP_VALUE + NEW_VALUE"), "(int) 13\n");
  EXPECT_TRUE(
      cpp->execute("#if __cplusplus >= 202002L\n#error old reference standard "
                   "leaked\n#endif\n"));
}

TEST(StartupFiles, LoadsNumberedScriptsInNumericOrderAndIgnoresOtherEntries) {
  TemporaryProject project;
  project.write(".cpp-repl/10-last.repl",
                "startup_order = startup_order * 10 + 4;\n");
  project.write(".cpp-repl/2-b.repl",
                "startup_order = startup_order * 10 + 3;\n");
  project.write(".cpp-repl/0002-a.repl",
                "startup_order = startup_order * 10 + 2;\n");
  project.write(".cpp-repl/0000-init.repl",
                "// startup\n\nint startup_order = 1;\n");
  project.write(".cpp-repl/9999999999999999999999999999999-final.repl",
                "startup_order = startup_order * 10 + 5;\n");
  for (const auto* name :
       {"notes.txt", "setup.repl", "-bad.repl", "1-.repl", ".1-hidden.repl",
        "1-wrong.cpp", "x-bad.repl", "3-subdirectory.repl/0-nested.repl"}) {
    project.write(fs::path(".cpp-repl") / name, "#error must not be loaded\n");
  }
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.loaded, 5u);
  EXPECT_EQ(result.failed, 0u) << output.str();
  EXPECT_EQ(cpp->evaluate("startup_order"), "(int) 12345\n");
  EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));
}

TEST(StartupFiles, UsesCompilationDatabaseAndMissingDirectoryIsSilent) {
  TemporaryProject project;
  const auto build = project.root() / "build";
  fs::create_directories(build);
  project.write("preload.cpp",
                "#if __cplusplus < 202002L\n#error requires C++20\n#endif\nint "
                "preloaded_database() { return PRELOAD_VALUE; }\n");
  project.write(".cpp-repl/0-files.repl", "%add preload.cpp\n");
  write_database(project,
                 arguments_entry(build, "../preload.cpp",
                                 {"clang++", "-std=c++20", "-DPRELOAD_VALUE=42",
                                  "-c", "../preload.cpp"}));
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto missing = load_startup_files(libs, *cpp, output,
                                          (project.root() / "absent").string());
  EXPECT_FALSE(missing.found);
  EXPECT_EQ(missing.failed, 0u);
  EXPECT_TRUE(output.str().empty());
  const auto loaded = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(loaded.loaded, 1u);
  EXPECT_EQ(loaded.failed, 0u) << output.str();
  EXPECT_EQ(cpp->evaluate("preloaded_database()"), "(int) 42\n");
  EXPECT_TRUE(cpp->execute(
      "#ifdef PRELOAD_VALUE\n#error preload flags leaked\n#endif\n"));
}

TEST(StartupFiles, ReferenceDefaultsAndExtraFlagsApplyBeforeLaterScripts) {
  TemporaryProject project;
  const auto build = project.root() / "build";
  fs::create_directories(build);
  project.write("include dir/config.hpp", "#define INCLUDED_VALUE 2\n");
  project.write("reference dir/reference.cpp", "int not_preloaded = 0;\n");
  project.write("config/files/first.hpp",
                "#if __cplusplus < 202002L || !DEBUG_SESSION\n"
                "#error missing startup defaults\n#endif\n"
                "#include <config.hpp>\n"
                "int configured_value = REFERENCE_VALUE + INCLUDED_VALUE;\n");
  project.write("config/files/second.cpp",
                "int configured_answer() { return configured_value; }\n");
  project.write(".cpp-repl/10-files.repl",
                "%add config/files/first.hpp config/files/second.cpp\n"
                "%compile-opts\n");
  project.write(".cpp-repl/2-options.repl",
                "%use-compile-opts \"reference dir/reference.cpp\"\n"
                "%add-compile-opt -DDEBUG_SESSION=1\n");
  write_database(
      project, arguments_entry(build, "../reference dir/reference.cpp",
                               {"clang++", "-std=c++20", "-DREFERENCE_VALUE=40",
                                "-I../include dir", "-c",
                                "../reference dir/reference.cpp"}));
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(result.failed, 0u) << output.str();
  EXPECT_EQ(result.loaded, 2u);
  EXPECT_EQ(result.commands, 4u);
  EXPECT_EQ(cpp->sources().size(), 2u);
  EXPECT_EQ(cpp->evaluate("configured_answer()"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("REFERENCE_VALUE + DEBUG_SESSION"), "(int) 41\n");
  EXPECT_NE(output.str().find("Reference file"), std::string::npos);
  EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));
}

TEST(StartupFiles, ErrorsReportScriptLocationsAndLaterScriptsStillRun) {
  TemporaryProject project;
  project.write(".cpp-repl/0-options.repl",
                "%set-compile-opts -DPRELOAD_VALUE=42\n");
  const auto failed_script = project.write(
      ".cpp-repl/2-errors.repl",
      "%not-a-command\n%use-compile-opts missing.cpp\n"
      "%add-compile-opt -fnot-a-real-option\n%add-path missing-libraries\n"
      "int = ;\n%add missing.cpp\n");
  project.write(".cpp-repl/10-later.repl",
                "int after_startup_errors = PRELOAD_VALUE;\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(result.failed, 6u) << output.str();
  EXPECT_EQ(result.loaded, 3u);
  EXPECT_EQ(cpp->evaluate("after_startup_errors"), "(int) 42\n");
  for (unsigned line = 1; line <= 6; ++line) {
    EXPECT_NE(output.str().find(failed_script.string() + ":" +
                                std::to_string(line) + ":"),
              std::string::npos);
  }
}

TEST(StartupFiles, MultilineCppAndHooksPreserveBlankLinesAndRestore) {
  TemporaryProject project;
  project.write(".cpp-repl/0-target.repl",
                "int startup_target(int n) {\n\nreturn n + 1;\n}\n"
                "int startup_hook_hits = 0;\n");
  project.write(
      ".cpp-repl/1-hooks.repl",
      "%pre startup_target(int n) cpp`\n"
      "\n#if 1\n++startup_hook_hits;\n#endif\n`\n"
      "%mock startup_target(int n) cpp`\nreturn n + 10;\n`\n"
      "%post startup_target(int n) cpp`result += startup_hook_hits;`\n"
      "int hooked_result = startup_target(2);\n");
  project.write(".cpp-repl/2-restore.repl",
                "%restore\nint restored_result = startup_target(2);\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(result.failed, 0u) << output.str();
  EXPECT_EQ(result.loaded, 3u);
  EXPECT_EQ(result.commands, 4u);
  EXPECT_EQ(cpp->evaluate("hooked_result"), "(int) 13\n");
  EXPECT_EQ(cpp->evaluate("restored_result"), "(int) 3\n");
  EXPECT_TRUE(cpp->active_hooks().empty());
}

TEST(StartupFiles, QuitStopsRemainingScripts) {
  TemporaryProject project;
  project.write(".cpp-repl/0-quit.repl", "%quit\n%add missing.cpp\n");
  project.write(".cpp-repl/1-skipped.repl", "#error must not run\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_TRUE(result.quit);
  EXPECT_EQ(result.loaded, 1u);
  EXPECT_EQ(result.commands, 1u);
  EXPECT_EQ(result.failed, 0u);
  EXPECT_TRUE(cpp->sources().empty());
}

TEST(StartupFiles, IncompleteInputsReportStartingLinesAndLaterScriptsStillRun) {
  TemporaryProject project;
  const auto hook = project.write(
      ".cpp-repl/0-hook.repl", "// startup\n%pre target(int n) cpp`\n\n++n;\n");
  const auto code =
      project.write(".cpp-repl/1-code.repl",
                    "// startup\nint incomplete(int n) {\n\nreturn n;\n");
  project.write(".cpp-repl/2-last.repl", "int after_incomplete = 42;\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(result.failed, 2u);
  EXPECT_NE(output.str().find(hook.string() + ":2: unterminated cpp` block"),
            std::string::npos);
  EXPECT_NE(output.str().find(code.string() + ":2: incomplete C++ input"),
            std::string::npos);
  EXPECT_EQ(cpp->evaluate("after_incomplete"), "(int) 42\n");
}

TEST(StartupFiles, RejectsAFileWhereTheDirectoryShouldBe) {
  TemporaryProject project;
  const auto file = project.write(".cpp-repl", "%quit\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(libs, *cpp, output, file.string());
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.failed, 1u);
  EXPECT_FALSE(result.quit);
  EXPECT_NE(output.str().find("must be a directory"), std::string::npos);
}

TEST(StartupFiles,
     UserScriptsRunBeforeTheNearestProjectWithProjectRelativePaths) {
  TemporaryProject project;
  const auto user = project.root() / "home/.cpp-repl";
  project.write("home/.cpp-repl/0-options.repl",
                "%set-compile-opts -DVALUE=1\n");
  project.write("home/.cpp-repl/99-user.repl", "int user_value = VALUE;\n");
  project.write("work/.cpp-repl/0-ignored.repl",
                "#error farther project must not load\n");
  project.write("work/project/.cpp-repl/2-options.repl",
                "%add-compile-opt -DVALUE=2\n");
  project.write("work/project/.cpp-repl/10-files.repl", "%add src/value.cpp\n");
  project.write("work/project/src/value.cpp",
                "int project_value() { return user_value + VALUE; }\n");
  const auto nested = project.root() / "work/project/src/nested";
  fs::create_directories(nested);
  CurrentDirectory cwd(nested);
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result =
      load_startup_directories(libs, *cpp, output, user.string());
  EXPECT_EQ(result.loaded, 4u);
  EXPECT_EQ(result.failed, 0u) << output.str();
  EXPECT_EQ(cpp->evaluate("user_value"), "(int) 1\n");
  EXPECT_EQ(cpp->evaluate("project_value()"), "(int) 3\n");
  EXPECT_EQ(cpp->evaluate("VALUE"), "(int) 2\n");
  EXPECT_EQ(fs::current_path(), fs::canonical(nested));
}

TEST(StartupFiles, UserDirectoryAndProjectAliasAreEachLoadedOnlyOnce) {
  for (const bool alias : {false, true}) {
    TemporaryProject project;
    const auto user = project.root() / "home/.cpp-repl";
    project.write("home/.cpp-repl/0-user.repl", "int startup_runs = 1;\n");
    const auto directory = project.root() / (alias ? "work" : "home/nested");
    fs::create_directories(directory);
    if (alias) fs::create_directory_symlink(user, directory / ".cpp-repl");
    CurrentDirectory cwd(directory);
    std::string error;
    auto cpp = create_interpreter(error);
    ASSERT_NE(cpp, nullptr) << error;
    LibraryManager libs;
    std::ostringstream output;
    const auto result =
        load_startup_directories(libs, *cpp, output, user.string());
    EXPECT_EQ(result.loaded, 1u);
    EXPECT_EQ(result.failed, 0u) << output.str();
    EXPECT_EQ(cpp->evaluate("startup_runs"), "(int) 1\n");
  }
}

TEST(StartupFiles, UserErrorsDoNotPreventProjectLoading) {
  TemporaryProject project;
  const auto user = project.write("home/.cpp-repl", "%quit\n");
  project.write("work/.cpp-repl/0-project.repl",
                "int project_after_error = 42;\n");
  CurrentDirectory cwd(project.root() / "work");
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result =
      load_startup_directories(libs, *cpp, output, user.string());
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(result.loaded, 1u);
  EXPECT_FALSE(result.quit);
  EXPECT_EQ(cpp->evaluate("project_after_error"), "(int) 42\n");
}

TEST(StartupFiles, UserQuitPreventsProjectLoading) {
  TemporaryProject project;
  const auto user = project.root() / "home/.cpp-repl";
  project.write("home/.cpp-repl/0-quit.repl", "%quit\n");
  project.write("work/.cpp-repl/0-skipped.repl", "#error must not run\n");
  CurrentDirectory cwd(project.root() / "work");
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result =
      load_startup_directories(libs, *cpp, output, user.string());
  EXPECT_TRUE(result.quit);
  EXPECT_EQ(result.failed, 0u);
  EXPECT_EQ(result.loaded, 1u);
}

TEST(StartupFiles, MissingDirectoriesAreSilentAndDiscoveryStopsAtHome) {
  TemporaryProject project;
  const auto user = project.root() / "home/.cpp-repl";
  project.write(".cpp-repl/0-outside-home.repl", "#error must not run\n");
  const auto nested = project.root() / "home/project";
  fs::create_directories(nested);
  CurrentDirectory cwd(nested);
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result =
      load_startup_directories(libs, *cpp, output, user.string());
  EXPECT_FALSE(result.found);
  EXPECT_EQ(result.failed, 0u);
  EXPECT_EQ(result.loaded, 0u);
  EXPECT_TRUE(output.str().empty());
}

TEST(StartupFiles, TypedStaticDefinitionsLoadBeforeDependentSourceFiles) {
  TemporaryProject project;
  project.write("state.hpp",
                "#pragma once\n#include <vector>\n"
                "namespace startup_storage { int seed() { return 21; } "
                "struct State { static std::vector<int> values; }; }\n");
  project.write("use.cpp",
                "int startup_storage_answer() { return "
                "startup_storage::State::values[1]; }\n");
  project.write(".cpp-repl/0-declarations.repl", "%add state.hpp\n");
  project.write(".cpp-repl/1-definitions.repl",
                "%symbols undefined name ^startup_storage::\n"
                "%define startup_storage::State::values cpp`\n"
                "std::vector<int>{seed(), seed() * 2}\n`\n");
  project.write(".cpp-repl/2-use.repl", "%add use.cpp\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  LibraryManager libs;
  std::ostringstream output;
  const auto result = load_startup_files(
      libs, *cpp, output, (project.root() / ".cpp-repl").string());
  EXPECT_EQ(result.failed, 0u) << output.str();
  EXPECT_EQ(cpp->evaluate("startup_storage_answer()"), "(int) 42\n");
  EXPECT_EQ(
      cpp->symbol_list("<session>", "^startup_storage::", error, false, true),
      "");
}

TEST(CompileOptions,
     FileAppendOverridesDatabaseAndGlobalAndSetReplacesDatabaseOptions) {
  TemporaryProject project;
  const auto first = project.write(
      "src/first.cpp",
      "#if __cplusplus < 202002L\n#error missing file standard\n#endif\nint "
      "file_first() { return VALUE + DATABASE_FLAG + GLOBAL_FLAG; }\n");
  const auto second =
      project.write("src/second.cpp",
                    "#if __cplusplus >= 202002L\n#error file standard "
                    "leaked\n#endif\nint file_second() { return VALUE; }\n");
  const auto third = project.write(
      "src/third.cpp",
      "#ifdef DATABASE_FLAG\n#error database options were not "
      "replaced\n#endif\nint file_third() { return VALUE + GLOBAL_FLAG; }\n");
  write_database(project,
                 arguments_entry(project.root(), "src/first.cpp",
                                 {"clang++", "-DVALUE=1", "-DDATABASE_FLAG=10",
                                  "-c", "src/first.cpp"}));
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(
      cpp->set_compile_options({"-DVALUE=2", "-DGLOBAL_FLAG=100"}, error))
      << error;
  ASSERT_TRUE(cpp->add_compile_options({"-std=c++20", "-DVALUE=3"}, error,
                                       "src/./first.cpp"))
      << error;
  auto added = cpp->add_source(first.string());
  ASSERT_TRUE(added.ok) << added.message;
  added = cpp->add_source(second.string());
  ASSERT_TRUE(added.ok) << added.message;
  ASSERT_TRUE(cpp->set_compile_options({"-DVALUE=4"}, error, "src/third.cpp"))
      << error;
  ASSERT_TRUE(cpp->add_compile_options({"-DVALUE=5"}, error, "src/third.cpp"))
      << error;
  added = cpp->add_source(third.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("file_first()"), "(int) 113\n");
  EXPECT_EQ(cpp->evaluate("file_second()"), "(int) 2\n");
  EXPECT_EQ(cpp->evaluate("file_third()"), "(int) 105\n");
  EXPECT_EQ(cpp->evaluate("VALUE"), "(int) 2\n");
  const auto description =
      cpp->describe_compile_options(error, "src/third.cpp");
  ASSERT_TRUE(description) << error;
  EXPECT_NE(description->find("replaced"), std::string::npos);
  EXPECT_NE(description->find("VALUE=5"), std::string::npos);
  EXPECT_NE(description->find(fs::canonical(third).string()),
            std::string::npos);
  const auto fourth =
      project.write("src/fourth.hpp",
                    "#ifdef DATABASE_FLAG\n#error clearing file flags did not "
                    "replace database defaults\n#endif\ninline int "
                    "file_fourth() { return VALUE; }\n");
  ASSERT_TRUE(cpp->set_compile_options({"-DVALUE=6"}, error, fourth.string()))
      << error;
  ASSERT_TRUE(cpp->set_compile_options({}, error, fourth.string())) << error;
  added = cpp->add_source(fourth.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("file_fourth()"), "(int) 2\n");
}

TEST(CompileOptions, ConfigurationAndInspectionDoNotExecuteForcedIncludes) {
  TemporaryProject project;
  const auto marker = project.root() / "initialized.txt";
  const auto forced = project.write(
      "forced.hpp",
      "#pragma once\n#include <cstdio>\n"
      "inline int flags_initialized = [] { auto* f = std::fopen(\"" +
          marker.string() +
          "\", \"w\"); if (f) std::fclose(f); return 31; }();\n");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int forced_flag_target(int x) { return x; }"));
  ASSERT_TRUE(cpp->set_compile_options({"-include", forced.string()}, error))
      << error;
  EXPECT_FALSE(fs::exists(marker));
  ASSERT_TRUE(cpp->describe_compile_options(error)) << error;
  EXPECT_FALSE(fs::exists(marker));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "forced_flag_target(int x)",
                            "return x + flags_initialized;", error))
      << error;
  EXPECT_TRUE(fs::exists(marker));
  EXPECT_EQ(cpp->evaluate("forced_flag_target(2)"), "(int) 33\n");
  ASSERT_TRUE(cpp->set_compile_options({}, error)) << error;
  ASSERT_TRUE(cpp->restore("", error)) << error;
  EXPECT_EQ(cpp->evaluate("forced_flag_target(2)"), "(int) 2\n");
}

TEST(
    CompileOptions,
    RelativeManualIncludesResolveFromCommandDirectoryAndInvalidUpdatesAreAtomic) {
  TemporaryProject project;
  fs::create_directories(project.root() / "build");
  project.write("manual include/value.hpp",
                "#pragma once\n#define MANUAL_HEADER_VALUE 42\n");
  const auto source =
      project.write("src/relative.cc",
                    "#include <value.hpp>\nint relative_options_value() { "
                    "return MANUAL_HEADER_VALUE; }\n");
  write_database(
      project,
      arguments_entry(project.root() / "build", "../src/relative.cc",
                      {"clang++", "-std=c++20", "-c", "../src/relative.cc"}),
      "build/compile_commands.json");
  CurrentDirectory cwd(project.root());
  std::string error;
  auto cpp = create_interpreter(error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->add_compile_options(
      {"-I", "manual include", "-DKEPT_FLAG=7"}, error))
      << error;
  const auto before = cpp->describe_compile_options(error);
  ASSERT_TRUE(before) << error;
  EXPECT_FALSE(cpp->set_compile_options({"-this-is-not-a-clang-flag"}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(cpp->describe_compile_options(error), before);
  EXPECT_FALSE(cpp->add_compile_options({"-I"}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(cpp->set_compile_options({"-fnot-a-valid-file-option"}, error,
                                        source.string()));
  EXPECT_FALSE(cpp->set_compile_options({"-D1INVALID_NAME=2"}, error));
  EXPECT_EQ(cpp->describe_compile_options(error), before);
  const auto added = cpp->add_source(source.string());
  ASSERT_TRUE(added.ok) << added.message;
  EXPECT_EQ(cpp->evaluate("relative_options_value() + KEPT_FLAG"),
            "(int) 49\n");
  EXPECT_EQ(fs::current_path(), fs::canonical(project.root()));
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
  project.write(
      "build/options.rsp",
      "-O2 -g -std=c++20 -DRESPONSE_VALUE=17 -c ../src/response.cxx -o "
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
