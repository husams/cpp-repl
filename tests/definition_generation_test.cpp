#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>
#include <unistd.h>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "definition_generation.hpp"
#include "library_manager.hpp"
#include "session_commands.hpp"
#include "startup_files.hpp"

namespace example {
namespace {
class DefinitionGeneration : public ::testing::Test {
protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "repl-defines-XXXXXX").string();
    auto* created = mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory = std::filesystem::canonical(created);
    std::string error;
    cpp = CppInterpreter::create({}, error);
    ASSERT_TRUE(cpp) << error;
  }

  void TearDown() override {
    cpp.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  std::string write(const std::string& name, const std::string& code) {
    const auto path = directory / name;
    std::ofstream(path) << code;
    return path.string();
  }

  bool command(const std::string& text, std::string* output = nullptr) {
    std::ostringstream out;
    auto result = execute_command(libs, *cpp, parse_command(text), out);
    if (output) {
      *output = out.str();
    }
    return result.ok;
  }

  std::filesystem::path directory;
  std::unique_ptr<CppInterpreter> cpp;
  LibraryManager libs;
};

TEST_F(DefinitionGeneration, ScansHeaderBeforeInclusionWithoutRunningInitializers) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto header =
      write("api.hpp", "#pragma once\nnamespace app { int adjust(int); extern int value; }\n"
                       "int initialized = (++sentinel, app::adjust(app::value));\n");
  std::string output;
  EXPECT_TRUE(command("%generate-defines \"" + header + "\"", &output)) << output;
  EXPECT_NE(output.find("%define app::adjust(int arg0)"), std::string::npos) << output;
  EXPECT_NE(output.find("%define app::value"), std::string::npos) << output;
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 0\n");
  EXPECT_TRUE(cpp->sources().empty());
  std::string error;
  EXPECT_FALSE(cpp->symbol_type("app::value", error));
}

TEST_F(DefinitionGeneration, StagesHeaderAndDefinitionsBeforeOneInitialization) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto header =
      write("api.hpp", "#pragma once\nnamespace app { int adjust(int); extern int value; }\n"
                       "int initialized = (++sentinel, app::adjust(app::value));\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + header + "\"", &output)) << output;
  ASSERT_TRUE(command("%define app::adjust(int x) cpp`return x + 7;`", &output)) << output;
  ASSERT_TRUE(command("%define app::value cpp`35`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("initialized"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 1\n");
  EXPECT_FALSE(command("%commit-defines"));
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 1\n");
}

TEST_F(DefinitionGeneration, FindsSourceOverloadsAndExcludesResolvedAndUnusedDeclarations) {
  ASSERT_TRUE(cpp->execute("int provided(int x) { return x; }"));
  const auto source = write(
      "source.cpp", "int unused(); int missing(int); double missing(double);\n"
                    "extern \"C\" int getpid();\n"
                    "int use() { return missing(provided(getpid())) + int(missing(2.0)); }\n");
  std::string output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_NE(output.find("%define missing(int arg0)"), std::string::npos) << output;
  EXPECT_NE(output.find("%define missing(double arg0)"), std::string::npos) << output;
  EXPECT_EQ(output.find("%define unused"), std::string::npos);
  EXPECT_EQ(output.find("%define provided"), std::string::npos);
  EXPECT_EQ(output.find("%define getpid"), std::string::npos);
  EXPECT_EQ(cpp->evaluate("provided(42)"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, FailedAnalysisAndMacrosDoNotEscapeIntoSession) {
  const auto source = write("invalid.cpp", "#define SCAN_ONLY 1\nnamespace scan { int bad = ; }\n");
  EXPECT_FALSE(command("%generate-defines \"" + source + "\""));
  ASSERT_TRUE(cpp->execute("#ifdef SCAN_ONLY\n#error leaked macro\n#endif\nint after_scan = 42;"));
  EXPECT_EQ(cpp->evaluate("after_scan"), "(int) 42\n");
  write("invalid.cpp", "#define SCAN_ONLY 1\nint needed(); int use() { return needed(); }\n");
  std::string first, second;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &first)) << first;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &second)) << second;
  EXPECT_EQ(first, second);
  EXPECT_TRUE(cpp->execute("#ifdef SCAN_ONLY\n#error leaked macro\n#endif\n"));
}

TEST_F(DefinitionGeneration, FailedCommitCanBeCompletedWithoutRunningSourceTwice) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto source = write(
      "source.cpp", "int first(); int second(); int result = (++sentinel, first() + second());");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define first() cpp`return 20;`", &output)) << output;
  EXPECT_FALSE(command("%commit-defines", &output));
  EXPECT_NE(output.find("remains unexecuted"), std::string::npos) << output;
  EXPECT_FALSE(cpp->execute("++sentinel;"));
  EXPECT_FALSE(cpp->evaluate("sentinel"));
  EXPECT_FALSE(command("%define second() cpp`return unknown_name;`", &output));
  ASSERT_TRUE(command("%define second() cpp`return 22;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 1\n");
}

TEST_F(DefinitionGeneration, CancellationRestoresHeaderAndAllowsPreparationAgain) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto header =
      write("api.hpp", "#pragma once\n#define PREPARED_MACRO 1\nnamespace app { int needed(); }\n"
                       "int result = (++sentinel, app::needed());\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + header + "\"", &output)) << output;
  ASSERT_TRUE(command("%define app::needed() cpp`return 1;`", &output)) << output;
  ASSERT_TRUE(command("%cancel-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 0\n");
  ASSERT_TRUE(cpp->execute("#ifdef PREPARED_MACRO\n#error leaked macro\n#endif\n"));
  ASSERT_TRUE(command("%prepare-defines \"" + header + "\"", &output)) << output;
  ASSERT_TRUE(command("%define app::needed() cpp`return 42;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 1\n");
}

TEST_F(DefinitionGeneration,
       DefinitionInitializersCanUsePreparedFunctionsBeforeSourceInitializers) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto source = write(
      "source.cpp", "int make_value() { ++sentinel; return 40; }\n"
                    "extern int missing_value; int result = (++sentinel, missing_value + 2);\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define missing_value cpp`make_value()`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 2\n");
}

TEST_F(DefinitionGeneration, SelectsHeaderCompilationContext) {
  const auto tu = write("context.cpp", "");
  const auto header = write("api.hpp", "#ifndef CHOSEN_VALUE\n#error wrong context\n#endif\n"
                                       "int needed(int); int result = needed(CHOSEN_VALUE);\n");
  std::string output;
  ASSERT_TRUE(command("%set-compile-opts \"" + tu + "\" -DCHOSEN_VALUE=40", &output)) << output;
  EXPECT_FALSE(command("%generate-defines \"" + header + "\""));
  ASSERT_TRUE(command("%generate-defines \"" + header + "\" --tu \"" + tu + "\"", &output))
      << output;
  EXPECT_NE(output.find("--tu"), std::string::npos);
  ASSERT_TRUE(command("%prepare-defines \"" + header + "\" --tu \"" + tu + "\"", &output))
      << output;
  ASSERT_TRUE(command("%define needed(int x) cpp`return x + 2;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
  write("api.hpp", "#ifndef CHOSEN_VALUE\n#error lost selected context\n#endif\n"
                   "int needed(int); int result = needed(CHOSEN_VALUE + 1);\n");
  ASSERT_TRUE(command("%reload \"" + header + "\"", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 43\n");
  write("api.hpp", "#ifndef CHOSEN_VALUE\n#error lost automatic context\n#endif\n"
                   "int needed(int); int result = needed(CHOSEN_VALUE + 2);\n");
  EXPECT_EQ(cpp->evaluate("result"), "(int) 44\n");
  std::string error;
  const auto inherited = cpp->generate_defines(header, error);
  ASSERT_TRUE(inherited) << error;
  EXPECT_NE(inherited->find("--tu \"" + tu + "\""), std::string::npos) << *inherited;
  EXPECT_NE(inherited->find("CHOSEN_VALUE=40"), std::string::npos) << *inherited;
}

TEST_F(DefinitionGeneration, ScansTemplateInstancesMemberQualifiersAndUnsupportedTargets) {
  const auto source =
      write("source.cpp",
            "template<class T> T generic(T);\n"
            "struct Worker { int run(int) const & noexcept; static int value; };\n"
            "int variadic(int, ...);\n"
            "int use(Worker& w) { return generic<int>(w.run(Worker::value)) + variadic(1, 2); }\n");
  std::string output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_NE(output.find("%define generic<int>(int arg0)"), std::string::npos) << output;
  EXPECT_NE(output.find("%define Worker::run(int arg0) const & noexcept"), std::string::npos)
      << output;
  EXPECT_NE(output.find("%define Worker::value"), std::string::npos) << output;
  EXPECT_NE(output.find("Unsupported:"), std::string::npos) << output;
  EXPECT_EQ(output.find("// %define variadic"), std::string::npos) << output;
}

TEST_F(DefinitionGeneration, CreatesAndUpdatesScriptsWithoutLosingUserEdits) {
  const auto source = write("source.cpp", "int needed(); int use() { return needed(); }");
  const auto output_path = (directory / "definitions.repl").string();
  const auto generate = "%generate-defines \"" + source + "\" --output \"" + output_path + "\"";
  std::string output;
  ASSERT_TRUE(command(generate, &output)) << output;
  EXPECT_FALSE(command(generate));
  std::ifstream input(output_path);
  std::string edited{std::istreambuf_iterator<char>(input), {}};
  const std::string placeholder = "// %define needed() cpp`/* function body */`";
  const auto pos = edited.find(placeholder);
  ASSERT_NE(pos, std::string::npos) << edited;
  edited.replace(pos, placeholder.size(), "%define needed() cpp`return 42;`");
  edited.insert(edited.find("%commit-defines"), "// My handwritten note.\n");
  std::ofstream(output_path) << edited;
  ASSERT_TRUE(command(generate + " --update", &output)) << output;
  std::ifstream updated(output_path);
  const std::string preserved{std::istreambuf_iterator<char>(updated), {}};
  EXPECT_NE(preserved.find("return 42;"), std::string::npos) << preserved;
  EXPECT_NE(preserved.find("My handwritten note."), std::string::npos) << preserved;
}

TEST_F(DefinitionGeneration, GeneratedScriptReplaysUsingNormalStartupLoader) {
  const auto source = write("source.cpp", "int needed(); int result = needed();");
  std::string error;
  auto script = cpp->generate_defines(source, error);
  ASSERT_TRUE(script) << error;
  const std::string placeholder = "// %define needed() cpp`/* function body */`";
  const auto pos = script->find(placeholder);
  ASSERT_NE(pos, std::string::npos) << *script;
  script->replace(pos, placeholder.size(), "%define needed() cpp`return 42;`");
  write("10-definitions.repl", *script);
  std::ostringstream output;
  const auto loaded = load_startup_files(libs, *cpp, output, directory.string());
  EXPECT_EQ(loaded.failed, 0u) << output.str();
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, RegenerationDisablesStaleSignaturesAndKeepsNewCandidates) {
  const auto source = write("source.cpp", "int needed(); int use() { return needed(); }");
  std::string error;
  const auto old = cpp->generate_defines(source, error);
  ASSERT_TRUE(old) << error;
  const auto output = write("10-definitions.repl", *old);
  write("source.cpp", "double needed(); int added(); double use() { return needed() + added(); }");
  const auto fresh = cpp->generate_defines(source, error);
  ASSERT_TRUE(fresh) << error;
  ASSERT_TRUE(write_definition_script(output, *fresh, true, error)) << error;
  std::ifstream input(output);
  const std::string content{std::istreambuf_iterator<char>(input), {}};
  EXPECT_NE(content.find("Stale definition"), std::string::npos) << content;
  EXPECT_NE(content.find("// %define added()"), std::string::npos) << content;
  EXPECT_NE(content.find("// Type: double (void)"), std::string::npos) << content;
  EXPECT_TRUE(write_definition_script(output, *fresh, true, error)) << error;
}

TEST_F(DefinitionGeneration, ScansEditedAddedFileWithoutReloadingLiveCode) {
  const auto source = write("source.cpp", "int value() { return 42; }");
  ASSERT_TRUE(cpp->add_source(source).ok);
  write("source.cpp", "int missing(); int value() { return missing(); }");
  std::string output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_NE(output.find("%define missing()"), std::string::npos) << output;
  EXPECT_FALSE(cpp->definitions_pending());
}

TEST_F(DefinitionGeneration, RejectsMalformedUpdateAndNeverOverwritesOrdinaryFiles) {
  const auto source = write("source.cpp", "int missing(); int use() { return missing(); }");
  std::string error;
  const auto script = cpp->generate_defines(source, error);
  ASSERT_TRUE(script) << error;
  const auto ordinary = write("ordinary.repl", "// handwritten\n");
  EXPECT_FALSE(write_definition_script(ordinary, *script, false, error));
  EXPECT_FALSE(write_definition_script(ordinary, *script, true, error));
  auto malformed = *script;
  malformed.erase(malformed.find("// cpp-repl:end"));
  const auto broken = write("broken.repl", malformed);
  EXPECT_FALSE(write_definition_script(broken, *script, true, error));
  const auto symlink = directory / "linked.repl";
  std::filesystem::create_symlink(broken, symlink);
  EXPECT_FALSE(write_definition_script(symlink.string(), *script, true, error));
}

TEST_F(DefinitionGeneration, DefinesConcreteTemplateInstancesAfterPreparation) {
  const auto source = write("source.cpp", "template<class T> T generic(T);\n"
                                          "template<class T> struct Box { T get() const; };\n"
                                          "int result = generic<int>(20) + Box<int>().get();\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define generic<int>(int value) cpp`return value;`", &output)) << output;
  ASSERT_TRUE(command("%define Box<int>::get() const cpp`return 22;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, InvalidVariableInitializerCanBeCorrectedBeforeCommit) {
  const auto source = write("source.cpp", "extern int missing; int result = missing + 2;\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  EXPECT_FALSE(command("%define missing cpp`unknown_initializer`", &output));
  ASSERT_TRUE(command("%define missing cpp`40`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, PreparesForcedIncludesWithoutRunningTheirInitializers) {
  ASSERT_TRUE(cpp->execute("int sentinel = 0;"));
  const auto forced =
      write("forced.hpp", "int needed(); inline int forced = (++sentinel, needed());\n");
  const auto source = write("source.cpp", "int result = forced;\n");
  std::string output;
  ASSERT_TRUE(command("%set-compile-opts \"" + source + "\" -include \"" + forced + "\"", &output))
      << output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_NE(output.find("%define needed()"), std::string::npos) << output;
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 0\n");
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define needed() cpp`return 42;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("sentinel"), "(int) 1\n");
}

TEST_F(DefinitionGeneration, FindsVirtualKeyMethodAndOtherMissingSlots) {
  const auto source =
      write("virtual.cpp",
            "struct Worker { virtual int first(); virtual int second(); }; Worker instance;\n");
  std::string output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_NE(output.find("%define Worker::first()"), std::string::npos) << output;
  EXPECT_NE(output.find("%define Worker::second()"), std::string::npos) << output;
  EXPECT_NE(output.find("owning method"), std::string::npos) << output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define Worker::first() cpp`return 40;`", &output)) << output;
  ASSERT_TRUE(command("%define Worker::second() cpp`return 2;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  ASSERT_TRUE(cpp->execute("int total(Worker* p) { return p->first() + p->second(); }"));
  EXPECT_EQ(cpp->evaluate("total(&instance)"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, StagedReloadCanBeCancelledAndThenCompleted) {
  const auto source = write("source.cpp", "int value() { return 10; }\n");
  ASSERT_TRUE(cpp->add_source(source).ok);
  ASSERT_TRUE(cpp->execute("int old_value() { return value(); }"));
  write("source.cpp", "int needed(); int value() { return needed(); }\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define needed() cpp`return 1;`", &output)) << output;
  ASSERT_TRUE(command("%cancel-defines", &output)) << output;
  // Avoid automatic reload while checking the restored lookup entry.
  write("source.cpp", "int value() { return 10; }\n");
  EXPECT_EQ(cpp->evaluate("value()"), "(int) 10\n");
  write("source.cpp", "int needed(); int value() { return needed(); }\n");
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%define needed() cpp`return 42;`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->sources().size(), 1u);
  EXPECT_EQ(cpp->evaluate("value()"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("old_value()"), "(int) 10\n");
}

TEST_F(DefinitionGeneration, ReconcilesRedeclaredPromptProviderDuringScanAndPreparation) {
  ASSERT_TRUE(cpp->execute("int provided(int x) { return x; }"));
  const auto source = write("source.cpp", "int provided(int); int result = provided(42);\n");
  std::string output;
  ASSERT_TRUE(command("%generate-defines \"" + source + "\"", &output)) << output;
  EXPECT_EQ(output.find("%define provided"), std::string::npos) << output;
  ASSERT_TRUE(command("%prepare-defines \"" + source + "\"", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  EXPECT_EQ(cpp->evaluate("result"), "(int) 42\n");
}

TEST_F(DefinitionGeneration, RejectsPathsThatCannotRoundTripThroughGeneratedCommands) {
  const auto path = write("back\\slash.cpp", "int missing(); int result = missing();\n");
  std::string error;
  EXPECT_FALSE(cpp->generate_defines(path, error));
  EXPECT_NE(error.find("unsupported source filename"), std::string::npos) << error;
}

TEST_F(DefinitionGeneration, PublishesCommittedDefinitionsToNativeConsumers) {
  const auto header = write(
      "api.hpp", "namespace generation_native { int missing(int); extern int value; }\n"
                 "inline int result = generation_native::missing(generation_native::value);\n");
  std::string output;
  ASSERT_TRUE(command("%prepare-defines \"" + header + "\"", &output)) << output;
  ASSERT_TRUE(command("%define generation_native::missing(int x) cpp`return x + 2;`", &output))
      << output;
  ASSERT_TRUE(command("%define generation_native::value cpp`40`", &output)) << output;
  ASSERT_TRUE(command("%commit-defines", &output)) << output;
  ASSERT_EQ(cpp->load_library(GENERATED_DEFINITIONS_NATIVE_LIBRARY), "");
  ASSERT_TRUE(cpp->execute("extern \"C\" int generated_native_answer();"));
  EXPECT_EQ(cpp->evaluate("generated_native_answer()"), "(int) 42\n");
}

TEST(DefinitionGenerationGrammar, ValidatesOptionsAndDoesNotTreatGenerationAsCppBody) {
  EXPECT_TRUE(parse_command("%generate-defines").error.size());
  EXPECT_TRUE(parse_command("%generate-defines api.hpp --output").error.size());
  EXPECT_TRUE(parse_command("%generate-defines api.hpp --tu a.cpp --tu b.cpp").error.size());
  EXPECT_TRUE(parse_command("%generate-defines api.hpp --update").error.size());
  EXPECT_TRUE(parse_command("%prepare-defines api.hpp --output x.repl").error.size());
  EXPECT_TRUE(parse_command("%commit-defines extra").error.size());
  EXPECT_TRUE(parse_command("%cancel-defines extra").error.size());
  EXPECT_TRUE(
      parse_command("%generate-defines \"source file.cpp\" --output \"my definitions.repl\"")
          .error.empty());
}
} // namespace
} // namespace example
