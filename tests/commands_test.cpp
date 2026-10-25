#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "commands.hpp"
#include "library_manager.hpp"
#include "inspection_output.hpp"

namespace example {
namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

TEST(ParseCommand, KnownCommandWithArgument) {
  const ParsedCommand cmd = parse_command("%lib  libz.dylib ");
  EXPECT_EQ(cmd.kind, CommandKind::Lib);
  EXPECT_EQ(cmd.name, "%lib");
  ASSERT_EQ(cmd.paths.size(), 1u);
  EXPECT_EQ(cmd.paths[0].value, "libz.dylib");
  EXPECT_EQ(cmd.name_begin, 0u);
  EXPECT_EQ(cmd.name_end, 4u);
  EXPECT_EQ(cmd.arg_begin, 6u);
  EXPECT_TRUE(cmd.error.empty());
}

TEST(ParseCommand, LeadingWhitespaceAndNoArgument) {
  const ParsedCommand cmd = parse_command("  %help");
  EXPECT_EQ(cmd.kind, CommandKind::Help);
  EXPECT_EQ(cmd.name_begin, 2u);
  EXPECT_TRUE(cmd.paths.empty());
  EXPECT_EQ(cmd.arg_begin, 7u);
  EXPECT_TRUE(cmd.error.empty());
}

std::vector<std::string> values(const ParsedCommand& cmd) {
  std::vector<std::string> out;
  for (const Operand& p : cmd.paths) out.push_back(p.value);
  return out;
}

// Text covered by the error span.
std::string error_text(std::string_view text, const ParsedCommand& cmd) {
  return std::string(text.substr(cmd.error_span.begin,
                                 cmd.error_span.end - cmd.error_span.begin));
}

TEST(Grammar, LibTakesBlankSeparatedPaths) {
  const std::string text = R"(%lib z  "/opt/my libs/libx.dylib" ./liby.so)";
  const ParsedCommand cmd = parse_command(text);
  EXPECT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(values(cmd),
            (std::vector<std::string>{"z", "/opt/my libs/libx.dylib", "./liby.so"}));
  EXPECT_EQ(text.substr(cmd.paths[1].span.begin,
                        cmd.paths[1].span.end - cmd.paths[1].span.begin),
            R"("/opt/my libs/libx.dylib")");
  EXPECT_TRUE(parse_command("%lib").error.empty());
}

TEST(Grammar, LibErrors) {
  const std::string unterminated = R"(%lib z "/opt/x)";
  ParsedCommand cmd = parse_command(unterminated);
  EXPECT_EQ(cmd.error, "unterminated quote");
  EXPECT_EQ(error_text(unterminated, cmd), R"("/opt/x)");

  EXPECT_EQ(parse_command(R"(%lib "a"b)").error, "expected a blank between paths");
  EXPECT_EQ(parse_command(R"(%lib "")").error, "empty path");
}

TEST(Grammar, AddPathTakesColonSeparatedPaths) {
  for (const char* text : {"%add-path /a:/b:/c", "%add-path /a : /b:/c "}) {
    const ParsedCommand cmd = parse_command(text);
    EXPECT_TRUE(cmd.error.empty()) << text << ": " << cmd.error;
    EXPECT_EQ(values(cmd), (std::vector<std::string>{"/a", "/b", "/c"})) << text;
    EXPECT_EQ(cmd.separators.size(), 2u) << text;
  }
  const ParsedCommand quoted = parse_command(R"(%add-path "/my dir":/b)");
  EXPECT_EQ(values(quoted), (std::vector<std::string>{"/my dir", "/b"}));
}

TEST(Grammar, AddPathErrors) {
  EXPECT_EQ(parse_command("%add-path").error, "missing path");
  EXPECT_EQ(parse_command("%add-path   ").error, "missing path");

  for (const char* text : {"%add-path /a:", "%add-path :/a", "%add-path /a::/b"}) {
    const ParsedCommand cmd = parse_command(text);
    EXPECT_EQ(cmd.error, "empty path in list") << text;
    EXPECT_EQ(error_text(text, cmd), ":") << text;
  }

  const std::string extra = "%add-path /a /b /c";
  const ParsedCommand cmd = parse_command(extra);
  EXPECT_EQ(cmd.error, "expected ':' between paths");
  EXPECT_EQ(error_text(extra, cmd), "/b /c");
}

TEST(Grammar, AddTakesBlankSeparatedSourceFiles) {
  ParsedCommand cmd = parse_command(R"(%add  a.cpp "my dir/b.cpp" ~/c.hpp)");
  EXPECT_EQ(cmd.kind, CommandKind::Add);
  EXPECT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(values(cmd), (std::vector<std::string>{"a.cpp", "my dir/b.cpp", "~/c.hpp"}));

  cmd = parse_command("%add");
  EXPECT_EQ(cmd.kind, CommandKind::Add);
  EXPECT_TRUE(cmd.error.empty());
  EXPECT_TRUE(cmd.paths.empty());

  EXPECT_EQ(parse_command(R"(%add "a.cpp)").error, "unterminated quote");
  EXPECT_EQ(parse_command("%add-path /tmp").kind, CommandKind::AddPath);  // not %add
}

TEST(Grammar, CompileOptionsSupportGlobalAndFileScopesWithoutSeparators) {
  auto cmd = parse_command("%compile-opts");
  EXPECT_EQ(cmd.kind, CommandKind::CompileOpts);
  EXPECT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_TRUE(cmd.paths.empty());
  cmd = parse_command("%compile-opts 'my sources/foo.cc'");
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(values(cmd), (std::vector<std::string>{"my sources/foo.cc"}));
  EXPECT_FALSE(parse_command("%compile-opts a.cpp b.cpp").error.empty());

  cmd = parse_command(
      R"(%add-compile-opt -I 'my include' -D'NAME="hello world"')");
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(cmd.kind, CommandKind::AddCompileOpt);
  EXPECT_TRUE(cmd.paths.empty());
  std::vector<std::string> flags;
  for (const auto& flag : cmd.compile_flags) flags.push_back(flag.value);
  EXPECT_EQ(flags, (std::vector<std::string>{"-I", "my include",
                                             "-DNAME=\"hello world\""}));

  cmd = parse_command(
      R"(%set-compile-opts "my sources/foo.cc" -std=c++20 -DVALUE=42)");
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(cmd.kind, CommandKind::SetCompileOpts);
  EXPECT_EQ(values(cmd), (std::vector<std::string>{"my sources/foo.cc"}));
  ASSERT_EQ(cmd.compile_flags.size(), 2u);
  EXPECT_EQ(cmd.compile_flags[0].value, "-std=c++20");
  EXPECT_EQ(cmd.compile_flags[1].value, "-DVALUE=42");
  EXPECT_TRUE(parse_command("%set-compile-opts").error.empty());
  EXPECT_TRUE(parse_command("%set-compile-opts foo.hpp").error.empty());
  EXPECT_FALSE(parse_command("%add-compile-opt").error.empty());
  EXPECT_FALSE(parse_command("%add-compile-opt foo.cpp").error.empty());
  EXPECT_FALSE(parse_command("%add-compile-opt -I 'broken").error.empty());
  EXPECT_FALSE(parse_command("%set-compile-opts foo.cpp stray.cpp -DOK=1")
                   .error.empty());
  EXPECT_FALSE(wants_more_input("%add-compile-opt -D'VALUE=(1)'"));
}

TEST(Grammar, UseCompileOptionsRequiresOneReferenceFilename) {
  const auto cmd = parse_command("%use-compile-opts 'my sources/point.cpp'");
  EXPECT_EQ(cmd.kind, CommandKind::UseCompileOpts);
  EXPECT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_EQ(values(cmd), (std::vector<std::string>{"my sources/point.cpp"}));
  EXPECT_TRUE(cmd.compile_flags.empty());
  EXPECT_FALSE(parse_command("%use-compile-opts").error.empty());
  EXPECT_FALSE(parse_command("%use-compile-opts ''").error.empty());
  EXPECT_FALSE(parse_command("%use-compile-opts a.cpp b.cpp").error.empty());
  EXPECT_FALSE(
      parse_command("%use-compile-opts a.cpp -DVALUE=1").error.empty());
  EXPECT_FALSE(parse_command("%use-compile-opts 'unfinished").error.empty());
}

TEST(Grammar, ClassTakesAnOptionalQualifiedName) {
  ParsedCommand cmd = parse_command("%class");
  EXPECT_EQ(cmd.kind, CommandKind::Class);
  EXPECT_TRUE(cmd.error.empty());
  EXPECT_FALSE(cmd.class_name.has_value());

  for (const char* name : {"Point", "geo::Vec2", "::std::string", "_x1"}) {
    cmd = parse_command(std::string("%class ") + name + " ");
    EXPECT_TRUE(cmd.error.empty()) << name << ": " << cmd.error;
    ASSERT_TRUE(cmd.class_name.has_value()) << name;
    EXPECT_EQ(cmd.class_name->value, name);
    EXPECT_EQ(cmd.class_name->span.begin, 7u);
  }
}

TEST(Grammar, ClassErrors) {
  for (const char* text : {"%class 1abc", "%class a::", "%class a:b", "%class a-b"}) {
    const ParsedCommand cmd = parse_command(text);
    EXPECT_EQ(cmd.error, "expected a class name") << text;
    EXPECT_EQ(error_text(text, cmd), std::string(text).substr(7)) << text;
  }
  const std::string text = "%class A B";
  const ParsedCommand cmd = parse_command(text);
  EXPECT_EQ(cmd.error, "%class takes one class name");
  EXPECT_EQ(error_text(text, cmd), "B");
}

TEST(Grammar, HelpAndQuitTakeNoArguments) {
  const std::string text = "%help me now ";
  const ParsedCommand cmd = parse_command(text);
  EXPECT_EQ(cmd.error, "%help takes no arguments");
  EXPECT_EQ(error_text(text, cmd), "me now");
  EXPECT_EQ(parse_command("%quit 1").error, "%quit takes no arguments");
}

TEST(Grammar, UnknownCommandSpansName) {
  const std::string text = "  %exit now";
  const ParsedCommand cmd = parse_command(text);
  EXPECT_EQ(cmd.kind, CommandKind::Unknown);
  EXPECT_EQ(error_text(text, cmd), "%exit");
}

TEST(ParseCommand, AllCommands) {
  EXPECT_EQ(parse_command("%add-path /tmp").kind, CommandKind::AddPath);
  EXPECT_EQ(parse_command("%quit").kind, CommandKind::Quit);
  EXPECT_EQ(parse_command("%exit").kind, CommandKind::Unknown);
  EXPECT_EQ(parse_command("%foo bar").kind, CommandKind::Unknown);
}

TEST(ParseCommand, WithoutPercentIsCpp) {
  for (const char* text : {"quit", "exit", "help", "int x = 1;", "", "   "}) {
    EXPECT_EQ(parse_command(text).kind, CommandKind::Cpp) << text;
    EXPECT_FALSE(is_command_line(text)) << text;
  }
}

TEST(WantsMoreInput, CommandsNeverContinue) {
  EXPECT_FALSE(wants_more_input("%lib don't"));
  EXPECT_FALSE(wants_more_input("%add-path ("));
  EXPECT_TRUE(wants_more_input("int main() {"));
  EXPECT_FALSE(wants_more_input("int main() { return 0; }"));
}

TEST(Grammar, HookCommandsKeepSignatureAndMultilineBody) {
  for (const char* name : {"%mock", "%pre", "%post"}) {
    const auto cmd = parse_command(std::string(name) +
        " int ns::Thing::value(int x) const cpp`\n  return x + 1;\n` ");
    ASSERT_TRUE(cmd.error.empty()) << cmd.error;
    ASSERT_TRUE(cmd.signature);
    ASSERT_TRUE(cmd.code);
    EXPECT_EQ(cmd.signature->value, "int ns::Thing::value(int x) const");
    EXPECT_EQ(cmd.code->value, "\n  return x + 1;\n");
    EXPECT_FALSE(cmd.incomplete);
  }
  EXPECT_EQ(parse_command("%mock int f() cpp``").code->value, "");
}

TEST(Grammar, HookDelimitersRespectCppLiteralsAndComments) {
  const auto cmd = parse_command("%mock const char* f() cpp`\n"
      "// a ` in a comment\nconst char* s = R\"tag(`)tag\"; return \"`\";\n`");
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  EXPECT_NE(cmd.code->value.find("return \"`\";"), std::string::npos);
  EXPECT_FALSE(wants_more_input("%pre int f() cpp`/* ` */ int x = 0;`"));
  EXPECT_TRUE(wants_more_input("%pre int f() cpp`// `\n"));
}

TEST(Grammar, HookErrorsAndContinuation) {
  EXPECT_FALSE(parse_command("%mock").error.empty());
  EXPECT_FALSE(parse_command("%pre cpp`x++;`").error.empty());
  EXPECT_FALSE(parse_command("%post int f() cpp`` extra").error.empty());
  EXPECT_TRUE(wants_more_input("%mock int f() cpp`"));
  EXPECT_TRUE(wants_more_input("%mock int f() cpp`return 1;\n"));
  EXPECT_FALSE(wants_more_input("%mock int f() cpp`return 1;`"));
  EXPECT_FALSE(wants_more_input("%mock int f()"));
}

TEST(Grammar, RestoreTakesASignatureWithoutACodeBlock) {
  const auto cmd = parse_command("  %restore  int Counter::add(int x) const  ");
  EXPECT_EQ(cmd.kind, CommandKind::Restore);
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  ASSERT_TRUE(cmd.signature);
  EXPECT_EQ(cmd.signature->value, "int Counter::add(int x) const");
  EXPECT_FALSE(cmd.code);
  EXPECT_FALSE(wants_more_input("%restore Counter::add(int x)"));
  for (const char* input : {"%restore", "%restore  "}) {
    const auto all = parse_command(input);
    EXPECT_EQ(all.kind, CommandKind::Restore);
    EXPECT_TRUE(all.error.empty());
    EXPECT_FALSE(all.signature);
    EXPECT_FALSE(wants_more_input(input));
  }
}

TEST(Grammar, HooksListsWithoutArguments) {
  const auto cmd = parse_command("  %hooks  ");
  EXPECT_EQ(cmd.kind, CommandKind::Hooks);
  EXPECT_TRUE(cmd.error.empty());
  EXPECT_FALSE(cmd.signature);
  EXPECT_FALSE(wants_more_input("%hooks"));
  EXPECT_EQ(parse_command("%hooks extra").error, "%hooks takes no arguments");
}

TEST(Grammar, InfoRequiresExactlyOneSymbolName) {
  for (const char* name : {"count", "demo::add", "::demo::add", "_ZN4demo3addEii", "Point::x"}) {
    const auto cmd = parse_command(std::string("  %info ") + name + "  ");
    ASSERT_TRUE(cmd.error.empty()) << cmd.error;
    EXPECT_EQ(cmd.kind, CommandKind::Info);
    ASSERT_TRUE(cmd.symbol);
    EXPECT_EQ(cmd.symbol->value, name);
  }
  EXPECT_EQ(parse_command("%info").error, "missing symbol name");
  EXPECT_EQ(parse_command("%info   ").error, "missing symbol name");
  EXPECT_EQ(parse_command("%info foo bar").error, "%info takes one symbol name");
  EXPECT_FALSE(wants_more_input("%info foo("));
}

TEST(Grammar, DemangleRequiresExactlyOneSymbolName) {
  for (const char* input : {"%demangle _Z3addii", "%demangle '__Z3addii'",
                             "%demangle \"Point::inc(int)\""}) {
    const auto cmd = parse_command(input);
    EXPECT_EQ(cmd.kind, CommandKind::Demangle);
    ASSERT_TRUE(cmd.symbol);
    EXPECT_TRUE(cmd.error.empty()) << cmd.error;
  }
  for (const char* input : {"%demangle", "%demangle ''", "%demangle \"\""})
    EXPECT_EQ(parse_command(input).error, "missing symbol name");
  EXPECT_EQ(parse_command("%demangle a b").error,
            "%demangle takes one symbol name");
  EXPECT_EQ(parse_command("%demangle 'x").error, "unterminated quote");
  EXPECT_FALSE(wants_more_input("%demangle _Zbroken("));
}

TEST(Dispatch, DemangleSupportsLinuxMacAndUnrecognizedNamesWithoutLibraries) {
  LibraryManager libs;
  for (const auto& names : std::vector<std::pair<std::string, std::string>>{
           {"_Z3addii", "add(int, int)"},
           {"__Z3addii", "add(int, int)"},
           {"_ZN5Point3incEi", "Point::inc(int)"},
           {"__ZN5Point3incEi", "Point::inc(int)"},
           {"printf", "printf"},
           {"'Point::inc(int)'", "Point::inc(int)"},
           {"_Zbroken", "_Zbroken"},
           {"__Zbroken", "__Zbroken"}}) {
    std::ostringstream output;
    bool ok = false;
    EXPECT_TRUE(dispatch(parse_command("%demangle " + names.first), libs,
                         output, &ok));
    EXPECT_TRUE(ok);
    EXPECT_EQ(output.str(), color_inspection(names.second + '\n',
                                            inspection_colors_enabled(), true));
  }
  EXPECT_TRUE(libs.loaded().empty());
}

TEST(Grammar, SymbolsAcceptsQuotedRegexesInEitherOrder) {
  const auto all = parse_command("  %symbols  ");
  EXPECT_EQ(all.kind, CommandKind::Symbols);
  EXPECT_TRUE(all.error.empty());
  EXPECT_FALSE(all.library_regex);
  EXPECT_FALSE(all.name_regex);
  EXPECT_FALSE(all.all_symbols);
  const std::string input = R"(%symbols name '^ns::Thing::add\(int, int\)$' lib "my lib.*")";
  const auto cmd = parse_command(input);
  ASSERT_TRUE(cmd.error.empty()) << cmd.error;
  ASSERT_TRUE(cmd.library_regex);
  ASSERT_TRUE(cmd.name_regex);
  EXPECT_EQ(cmd.library_regex->value, "my lib.*");
  EXPECT_EQ(cmd.name_regex->value, R"(^ns::Thing::add\(int, int\)$)");
  EXPECT_EQ(input.substr(cmd.name_regex->span.begin,
                        cmd.name_regex->span.end - cmd.name_regex->span.begin),
            R"('^ns::Thing::add\(int, int\)$')");
  EXPECT_EQ(parse_command(R"(%symbols name "")").name_regex->value, "");
  EXPECT_EQ(parse_command("%symbols lib libpoints name Point::").library_regex->value,
            "libpoints");
  EXPECT_FALSE(wants_more_input("%symbols name ["));
  EXPECT_EQ(parse_command("%symbols name lib").name_regex->value, "lib");
  EXPECT_EQ(parse_command("%symbols lib name").library_regex->value, "name");
  EXPECT_EQ(parse_command("%symbols name all").name_regex->value, "all");
}

TEST(Grammar, SymbolsAllIncludesSystemLibrariesWithFiltersInEitherOrder) {
  for (const std::string input : {"%symbols all", "%symbols all lib libc name malloc",
                                  "%symbols lib libc all name malloc",
                                  "%symbols name malloc lib libc all"}) {
    const auto cmd = parse_command(input);
    ASSERT_TRUE(cmd.error.empty()) << cmd.error;
    ASSERT_TRUE(cmd.all_symbols);
    EXPECT_EQ(input.substr(cmd.all_symbols->begin,
                           cmd.all_symbols->end - cmd.all_symbols->begin), "all");
  }
  EXPECT_FALSE(parse_command("%symbols all all").error.empty());
  EXPECT_FALSE(parse_command("%symbols all lib libc all").error.empty());
}

TEST(Grammar, LibraryAndSymbolInspectionAcceptsQuotedNames) {
  EXPECT_EQ(parse_command("%libs").kind, CommandKind::Libs);
  EXPECT_TRUE(parse_command("%libs").error.empty());
  const auto libraries = parse_command("%libs 'libpoints|libc[.]'");
  ASSERT_TRUE(libraries.symbol);
  EXPECT_EQ(libraries.symbol->value, "libpoints|libc[.]");
  EXPECT_EQ(parse_command("%libinfo libpoints").kind, CommandKind::LibInfo);
  const auto symbol = parse_command("%syminfo 'Point::inc(int, int)'");
  EXPECT_EQ(symbol.kind, CommandKind::Info);
  ASSERT_TRUE(symbol.symbol);
  EXPECT_EQ(symbol.symbol->value, "Point::inc(int, int)");
  EXPECT_EQ(parse_command("%info \"Point::inc(int)\"").symbol->value, "Point::inc(int)");
  for (const char* input : {"%libinfo", "%libinfo 'x", "%libs x y", "%syminfo", "%syminfo x y"})
    EXPECT_FALSE(parse_command(input).error.empty()) << input;
}

TEST(Grammar, LibsAllIncludesSystemLibrariesAndKeepsTheOptionalRegex) {
  EXPECT_FALSE(parse_command("%libs").all_libraries);
  const auto all = parse_command("  %libs all  ");
  EXPECT_EQ(all.kind, CommandKind::Libs);
  EXPECT_TRUE(all.error.empty());
  ASSERT_TRUE(all.all_libraries);
  EXPECT_FALSE(all.symbol);
  EXPECT_EQ(all.all_libraries->begin, 8u);
  EXPECT_EQ(all.all_libraries->end, 11u);
  const auto filtered = parse_command(R"(%libs all 'libc[.]|my lib')");
  ASSERT_TRUE(filtered.error.empty()) << filtered.error;
  EXPECT_TRUE(filtered.all_libraries);
  ASSERT_TRUE(filtered.symbol);
  EXPECT_EQ(filtered.symbol->value, "libc[.]|my lib");
  EXPECT_EQ(filtered.symbol->span.begin, 10u);
  const auto quoted = parse_command("%libs 'all'");
  EXPECT_FALSE(quoted.all_libraries);
  ASSERT_TRUE(quoted.symbol);
  EXPECT_EQ(quoted.symbol->value, "all");
  EXPECT_FALSE(parse_command("%libs allocation").all_libraries);
  for (const char* input : {"%libs all x y", "%libs all 'x", "%libs all \"x\"y"}) {
    EXPECT_FALSE(parse_command(input).error.empty()) << input;
    EXPECT_FALSE(wants_more_input(input)) << input;
  }
}

TEST(InspectionOutput, AddsColorWithoutChangingPlainTextContent) {
  const std::string info = "Symbol: demo::add\n  Kind    : function\n  Address : 0x123\n";
  EXPECT_EQ(color_inspection(info, false), info);
  const auto colored = color_inspection(info, true);
  EXPECT_NE(colored.find("\033[36m"), std::string::npos);
  EXPECT_NE(colored.find("\033[33m0x123\033[0m"), std::string::npos);
  EXPECT_EQ(color_inspection("demo::add(int)\n", true, true), "\033[36mdemo::add(int)\033[0m\n");
  EXPECT_EQ(color_inspection("", true, true), "");
}

TEST(Grammar, SymbolsRejectsInvalidOptionsWithoutContinuingInput) {
  for (const char* input : {"%symbols name", "%symbols lib name x",
                           "%symbols unknown x", "%symbols extra", "%symbols --name x",
                           "%symbols name x name y", "%symbols lib x lib y",
                           "%symbols name 'x", "%symbols name \"x\"y"}) {
    EXPECT_FALSE(parse_command(input).error.empty()) << input;
    EXPECT_FALSE(wants_more_input(input)) << input;
  }
}

TEST(Grammar, CallerFiltersSelectSignaturesOutsideParameterLists) {
  for (const char* name : {"%mock", "%pre", "%post"}) {
    const auto cmd = parse_command(std::string(name) +
        " int Calc::add(int in) in int Calc::inc(int n) const cpp`return in;`");
    ASSERT_TRUE(cmd.error.empty()) << cmd.error;
    ASSERT_TRUE(cmd.signature);
    ASSERT_TRUE(cmd.caller);
    EXPECT_EQ(cmd.signature->value, "int Calc::add(int in)");
    EXPECT_EQ(cmd.caller->value, "int Calc::inc(int n) const");
    EXPECT_EQ(cmd.code->value, "return in;");
  }
  const auto restore = parse_command("%restore add(int x) in inc(int n)");
  ASSERT_TRUE(restore.error.empty());
  ASSERT_TRUE(restore.caller);
  EXPECT_EQ(restore.caller->value, "inc(int n)");
  EXPECT_FALSE(parse_command("%mock in inc(int x) cpp`return 1;`").error.empty());
  EXPECT_FALSE(parse_command("%mock add(int x) in cpp`return 1;`").error.empty());
  EXPECT_FALSE(parse_command("%restore add(int x) in").error.empty());
  EXPECT_FALSE(parse_command("%mock int in (int n) cpp`return n;`").caller);
  EXPECT_TRUE(wants_more_input("%mock add(int x) in inc(int n) cpp`\n"));
}

TEST(Dispatch, QuitStopsAndHelpListsCommands) {
  LibraryManager libs;
  std::ostringstream out;
  EXPECT_FALSE(dispatch(parse_command("%quit"), libs, out));
  EXPECT_TRUE(dispatch(parse_command("%help"), libs, out));
  for (const std::string& name : command_names()) {
    EXPECT_NE(out.str().find(name), std::string::npos) << name;
  }
  EXPECT_EQ(command_names().size(), 22u);
}

TEST(Dispatch, PathsShowsConfiguredDirectoriesAndLoaderSettings) {
  LibraryManager libraries;
  EXPECT_EQ(parse_command("%paths").kind, CommandKind::Paths);
  EXPECT_TRUE(parse_command("%paths").error.empty());
  EXPECT_FALSE(parse_command("%paths extra").error.empty());
  EXPECT_FALSE(wants_more_input("%paths ("));
  const auto directory = std::filesystem::canonical(std::filesystem::temp_directory_path()).string();
  ASSERT_TRUE(libraries.add_path(directory).ok);
  std::ostringstream output;
  EXPECT_TRUE(dispatch(parse_command("%paths"), libraries, output));
  EXPECT_EQ(output.str().find("Library search paths:\n"), 0u);
  EXPECT_NE(output.str().find(directory), std::string::npos);
  EXPECT_NE(output.str().find("Dependency fallback"), std::string::npos);
  EXPECT_NE(output.str().find("Native loader"), std::string::npos);
#ifdef __APPLE__
  EXPECT_NE(output.str().find("DYLD_LIBRARY_PATH"), std::string::npos);
#else
  EXPECT_NE(output.str().find("LD_LIBRARY_PATH"), std::string::npos);
#endif
  EXPECT_EQ(libraries.search_paths(), std::vector<std::string>{directory});
}

TEST(Dispatch, UnknownCommandSuggestsHelp) {
  LibraryManager libs;
  std::ostringstream out;
  EXPECT_TRUE(dispatch(parse_command("%exit"), libs, out));
  EXPECT_EQ(out.str(), "error: unknown command '%exit' (try %help)\n");
}

TEST(Dispatch, AddPathAddsEachExistingDir) {
  LibraryManager libs;
  std::ostringstream out;
  dispatch(parse_command("%add-path /no/such/dir"), libs, out);
  EXPECT_EQ(out.str().rfind("error: ", 0), 0u);
  EXPECT_TRUE(libs.search_paths().empty());

  const std::string tmp = std::filesystem::temp_directory_path().string();
  out.str("");
  dispatch(parse_command("%add-path " + tmp + ":/no/such/dir:/"), libs, out);
  EXPECT_EQ(out.str().find("added: "), 0u) << out.str();
  EXPECT_NE(out.str().find("error: no such directory: /no/such/dir"), std::string::npos);
  EXPECT_EQ(libs.search_paths().size(), 2u);
}

TEST(Dispatch, GrammarErrorPrintsUsageAndRunsNothing) {
  LibraryManager libs;
  std::ostringstream out;
  EXPECT_TRUE(dispatch(parse_command("%add-path /:"), libs, out));
  EXPECT_EQ(out.str(), "error: empty path in list\nusage: %add-path <path[:path...]>\n");
  EXPECT_TRUE(libs.search_paths().empty());

  out.str("");
  EXPECT_TRUE(dispatch(parse_command("%quit now"), libs, out));
  EXPECT_EQ(out.str(), "error: %quit takes no arguments\nusage: %quit\n");
}

TEST(LibraryManager, Candidates) {
  LibraryManager libs;
  EXPECT_EQ(libs.candidates("/abs/libx.dylib"), std::vector<std::string>{"/abs/libx.dylib"});
  const auto names = libs.candidates("foo");
  EXPECT_TRUE(contains(names, "foo"));
  EXPECT_TRUE(contains(names, "libfoo.dylib"));
  EXPECT_TRUE(contains(names, "libfoo.so"));
}

TEST(Dispatch, LibReportsMissingLibrary) {
  LibraryManager libs;
  std::ostringstream out;
  dispatch(parse_command("%lib no_such_library_xyz"), libs, out);
  EXPECT_EQ(out.str().rfind("error: ", 0), 0u);
  EXPECT_TRUE(libs.loaded().empty());
}

#ifdef __APPLE__
TEST(Dispatch, LibLoadsSystemLibraries) {
  LibraryManager libs;
  std::ostringstream out;
  dispatch(parse_command("%lib z c++"), libs, out);
  EXPECT_EQ(out.str().find("error: "), std::string::npos) << out.str();
  ASSERT_EQ(libs.loaded().size(), 2u);

  out.str("");
  dispatch(parse_command("%lib"), libs, out);
  EXPECT_NE(out.str().find(libs.loaded().front().path), std::string::npos);
}
#endif

}  // namespace
}  // namespace example
