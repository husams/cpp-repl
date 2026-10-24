#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef CPP_REPL_NATIVE_CALL_PATCHES
#include <dlfcn.h>
#endif

#include <gtest/gtest.h>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "cpp_symbols.hpp"
#include "library_manager.hpp"
#include "loaded_symbols.hpp"
#include "runtime_inspection.hpp"

namespace example {
namespace {

// One interpreter for the whole suite: creating one takes a while, and the
// tests also show that state carries over from one input to the next.
class CppInterpreterTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    std::string error;
    cpp_ = CppInterpreter::create({}, error).release();
    ASSERT_NE(cpp_, nullptr) << error;
  }
  static void TearDownTestSuite() {
    delete cpp_;
    cpp_ = nullptr;
  }

  static CppInterpreter* cpp_;
};

CppInterpreter* CppInterpreterTest::cpp_ = nullptr;

TEST_F(CppInterpreterTest, KeepsDeclarationsAcrossInputs) {
  EXPECT_EQ(cpp_->evaluate("int twice(int x) { return x * 2; }"), "");
  EXPECT_EQ(cpp_->evaluate("int n = 21;"), "");
  EXPECT_EQ(cpp_->evaluate("twice(n)"), "(int) 42\n");
  EXPECT_EQ(cpp_->evaluate("twice(n);"), "");  // ';' suppresses the value
}

TEST_F(CppInterpreterTest, FindsStandardLibraryHeaders) {
  EXPECT_TRUE(cpp_->execute("#include <vector>\n"));
  EXPECT_TRUE(cpp_->execute("#include <string>\n"));
  EXPECT_TRUE(cpp_->execute("std::vector<int> v{1, 2, 3};"));
  EXPECT_EQ(cpp_->evaluate("v.size()"), "(unsigned long) 3\n");
  EXPECT_TRUE(cpp_->execute("std::string s = std::string(\"ab\") + \"c\";"));
  EXPECT_EQ(cpp_->evaluate("s.c_str()"), "(const char *) \"abc\"\n");
}

TEST_F(CppInterpreterTest, StaysUsableAfterAParseError) {
  EXPECT_FALSE(cpp_->execute("int broken = ;"));
  EXPECT_EQ(cpp_->evaluate("no_such_name + 1"), std::nullopt);
  EXPECT_EQ(cpp_->evaluate("1 + 2"), "(int) 3\n");
}

TEST_F(CppInterpreterTest, StaysUsableAfterALinkError) {
  EXPECT_TRUE(cpp_->execute("int never_defined();"));
  EXPECT_FALSE(cpp_->execute("int use = never_defined();"));  // symbol not found
  EXPECT_EQ(cpp_->evaluate("2 + 3"), "(int) 5\n");
  EXPECT_TRUE(cpp_->execute("int use = 7;"));  // the failed input was undone
  EXPECT_EQ(cpp_->evaluate("use"), "(int) 7\n");
}

TEST_F(CppInterpreterTest, RejectsTopLevelReturnInsteadOfCrashing) {
  EXPECT_FALSE(cpp_->execute("return 1;"));
  EXPECT_FALSE(cpp_->execute("if (true) return;"));
  // LLVM 21/22's Itanium mangler can crash on an unnamed lambda directly in
  // a top-level statement (upstream llvm-project #217041). A named lambda
  // still verifies that the return guard permits returns inside functions.
  EXPECT_TRUE(cpp_->execute("auto return_lambda = [] { return 4; };"));
  EXPECT_EQ(cpp_->evaluate("return_lambda()"), "(int) 4\n");
}

TEST_F(CppInterpreterTest, MultilineInput) {
  EXPECT_TRUE(cpp_->execute("struct Point {\n  int x, y;\n  int sum() const { return x + y; }\n};"));
  EXPECT_EQ(cpp_->evaluate("Point{3, 4}.sum()"), "(int) 7\n");
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

TEST_F(CppInterpreterTest, CompletesSessionNames) {
  ASSERT_TRUE(cpp_->execute("int lab_counter = 0;"));
  ASSERT_TRUE(cpp_->execute("int lab_total(int a, int b) { return a + b; }"));
  const CppInterpreter::Completions c = cpp_->complete("lab_t");
  EXPECT_EQ(c.prefix, "lab_t");
  EXPECT_EQ(c.names, std::vector<std::string>{"lab_total"});
  EXPECT_TRUE(has(cpp_->complete("int x = lab_").names, "lab_counter"));
  EXPECT_TRUE(has(cpp_->complete("constex").names, "constexpr"));  // keywords too
}

TEST_F(CppInterpreterTest, CompletesMembersIncludingFields) {
  ASSERT_TRUE(cpp_->execute(
      "struct Shape { double area_cache; virtual double area() const { return 0; } };\n"
      "struct Rect : Shape { double width, height; Rect* next = nullptr; "
      "double area() const override { return width * height; } };\n"
      "Rect rect;\n"
      "Rect* rp = &rect;"));
  CppInterpreter::Completions c = cpp_->complete("rect.");
  EXPECT_EQ(c.prefix, "");
  for (const char* m : {"area", "area_cache", "height", "next", "width"}) {
    EXPECT_TRUE(has(c.names, m)) << m;
  }
  c = cpp_->complete("rp->wi");
  EXPECT_EQ(c.prefix, "wi");
  EXPECT_EQ(c.names, std::vector<std::string>{"width"});
  EXPECT_TRUE(has(cpp_->complete("rect.next->he").names, "height"));
}

TEST_F(CppInterpreterTest, DescribesUserClasses) {
  ASSERT_TRUE(cpp_->execute(
      "namespace geo { struct Vec2 { float x = 0, y = 0; float dot(const Vec2& o) const "
      "{ return x * o.x + y * o.y; } static int count; }; }\n"
      "class Account { public: explicit Account(int id) : id_(id) {} int id() const "
      "{ return id_; } private: int id_; bool open_ = true; };"));
  const std::vector<std::string> names = cpp_->class_names();
  EXPECT_TRUE(has(names, "geo::Vec2"));
  EXPECT_TRUE(has(names, "Account"));
  EXPECT_FALSE(has(names, "std::string"));  // header classes are not listed

  std::string error;
  const std::optional<std::string> vec = cpp_->describe_class("geo::Vec2", error);
  ASSERT_TRUE(vec.has_value()) << error;
  EXPECT_NE(vec->find("struct geo::Vec2 {  // size 8, align 4"), std::string::npos);
  EXPECT_NE(vec->find("float x = 0;"), std::string::npos);
  EXPECT_NE(vec->find("// +4, 4 bytes"), std::string::npos);
  EXPECT_NE(vec->find("float dot(const Vec2 &o) const;"), std::string::npos);
  EXPECT_NE(vec->find("static int count;"), std::string::npos);

  const std::optional<std::string> account = cpp_->describe_class("Account", error);
  ASSERT_TRUE(account.has_value()) << error;
  EXPECT_NE(account->find("private:"), std::string::npos);
  EXPECT_NE(account->find("explicit Account(int id);"), std::string::npos);

  EXPECT_EQ(cpp_->describe_class("NoSuchClass", error), std::nullopt);
  EXPECT_EQ(error, "no class named 'NoSuchClass' (try %class)");
}

TEST_F(CppInterpreterTest, DescribesBasesAndVptr) {
  ASSERT_TRUE(cpp_->execute("struct Base { virtual ~Base() = default; int b; };\n"
                            "struct Derived : public Base { int d; };"));
  std::string error;
  const std::optional<std::string> d = cpp_->describe_class("Derived", error);
  ASSERT_TRUE(d.has_value()) << error;
  EXPECT_NE(d->find("struct Derived : public Base {  // size 16, align 8"), std::string::npos);
  EXPECT_NE(d->find("// +0  base Base (16 bytes)"), std::string::npos);
  EXPECT_NE(d->find("int d;"), std::string::npos);
}

// A scratch directory with C++ sources, removed afterwards.
class SourceDir {
 public:
  SourceDir() {
    dir_ = std::filesystem::temp_directory_path() /
           ("cpp_repl_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir_);
  }
  ~SourceDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string write(const std::string& name, const std::string& text) const {
    const std::filesystem::path file = dir_ / name;
    std::ofstream(file) << text;
    return file.string();
  }

 private:
  std::filesystem::path dir_;
};

TEST_F(CppInterpreterTest, AddSourceParsesAndRunsAFile) {
  SourceDir dir;
  dir.write("shape.hpp", "#pragma once\nstruct Circle { double r; double area() const; };\n");
  const std::string source = dir.write("shape.cpp",
                                       "#include \"shape.hpp\"\n"
                                       "double Circle::area() const { return 3 * r * r; }\n"
                                       "int added_value = 40 + 2;\n");
  const CppInterpreter::AddResult r = cpp_->add_source(source);
  ASSERT_TRUE(r.ok) << r.message;
  EXPECT_EQ(r.message.rfind("added: ", 0), 0u);
  EXPECT_EQ(cpp_->evaluate("added_value"), "(int) 42\n");
  EXPECT_EQ(cpp_->evaluate("Circle{2}.area()"), "(double) 12.0\n");
  EXPECT_EQ(cpp_->sources().size(), 1u);

  // Classes from the file's own (non-system) headers are user classes too.
  EXPECT_TRUE(has(cpp_->class_names(), "Circle"));
  std::string error;
  const std::optional<std::string> circle = cpp_->describe_class("Circle", error);
  ASSERT_TRUE(circle.has_value()) << error;
  EXPECT_NE(circle->find("double area() const;"), std::string::npos);

  EXPECT_FALSE(cpp_->add_source(source).ok);  // once only
  EXPECT_EQ(cpp_->add_source(source).message, "already added: " + cpp_->sources()[0]);
}

TEST_F(CppInterpreterTest, AddSourceReportsErrors) {
  EXPECT_EQ(cpp_->add_source("/no/such/file.cpp").message, "no such file: /no/such/file.cpp");
  SourceDir dir;
  const std::string bad = dir.write("bad.cpp", "struct Broken { int x }\n");
  const CppInterpreter::AddResult r = cpp_->add_source(bad);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.message.rfind("could not add ", 0), 0u);
  EXPECT_EQ(cpp_->evaluate("6 * 7"), "(int) 42\n");  // still usable

  const std::string good = dir.write("good.cpp", "struct FromFile { int a; };\n");
  ASSERT_TRUE(cpp_->add_source(good).ok);
  EXPECT_TRUE(has(cpp_->class_names(), "FromFile"));
}

// Returns the value of an aligned metadata field without depending on an
// address or input_line_N number chosen by the JIT.
std::string info_field(const std::string& text, const std::string& label) {
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    const auto colon = line.find(" : ");
    if (colon == std::string::npos) continue;
    const auto end = line.find_last_not_of(' ', colon - 1);
    if (line.substr(2, end - 1) == label) return line.substr(colon + 3);
  }
  return "";
}

TEST_F(CppInterpreterTest, SymbolInfoShowsOverloadsAndMangledLookup) {
  ASSERT_TRUE(cpp_->execute("namespace info_demo { int add(int a, int b); }"));
  ASSERT_TRUE(cpp_->execute("namespace info_demo { int add(int a, int b) { return a+b; } "
                           "double add(double a, double b) { return a+b; } }"));
  std::string error;
  const auto all = cpp_->symbol_info("info_demo::add", error);
  ASSERT_TRUE(all.has_value()) << error;
  EXPECT_EQ(all->find("2 matches for 'info_demo::add'\n\n"), 0u);
  EXPECT_NE(all->find("int info_demo::add(int a, int b)"), std::string::npos) << *all;
  EXPECT_NE(all->find("double info_demo::add(double a, double b)"), std::string::npos);
  const auto mangled = info_field(*all, "Mangled");
  ASSERT_FALSE(mangled.empty());
  const auto single = cpp_->symbol_info(mangled, error);
  ASSERT_TRUE(single.has_value()) << error;
  EXPECT_EQ(single->find("Symbol: info_demo::add\n"), 0u);
  EXPECT_EQ(single->find("matches for"), std::string::npos);
  EXPECT_EQ(info_field(*single, "Kind"), "function");
  EXPECT_EQ(info_field(*single, "Definition"), "yes");
  EXPECT_EQ(info_field(*single, "Address").find("0x"), 0u);
  EXPECT_EQ(info_field(*single, "Original").find("0x"), 0u);
  EXPECT_EQ(info_field(*single, "Location").find("input_line_"), 0u);
  std::istringstream lines(*single);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.compare(0, 2, "  ") == 0) EXPECT_EQ(line.find(" : "), 13u) << line;
  }
}

TEST_F(CppInterpreterTest, SymbolInfoDescribesVariablesTypesAndFields) {
  ASSERT_TRUE(cpp_->execute("int info_count = 7; "
      "struct InfoBox { int value; int get() const { return value; } }; "
      "using InfoAlias = InfoBox;"));
  std::string error;
  auto variable = cpp_->symbol_info("info_count", error);
  ASSERT_TRUE(variable.has_value()) << error;
  EXPECT_EQ(info_field(*variable, "Kind"), "variable");
  EXPECT_EQ(info_field(*variable, "Type"), "int");
  EXPECT_EQ(info_field(*variable, "Address").find("0x"), 0u);
  auto record = cpp_->symbol_info("InfoBox", error);
  ASSERT_TRUE(record.has_value()) << error;
  EXPECT_EQ(info_field(*record, "Kind"), "struct");
  EXPECT_EQ(info_field(*record, "Size"), "4 bytes");
  EXPECT_EQ(info_field(*record, "Address"), "not applicable");
  auto alias = cpp_->symbol_info("InfoAlias", error);
  ASSERT_TRUE(alias.has_value()) << error;
  EXPECT_EQ(info_field(*alias, "Kind"), "type alias");
  EXPECT_EQ(info_field(*alias, "Type"), "InfoBox");
  auto field = cpp_->symbol_info("InfoBox::value", error);
  ASSERT_TRUE(field.has_value()) << error;
  EXPECT_EQ(info_field(*field, "Kind"), "field");
  EXPECT_EQ(info_field(*field, "Offset"), "0 bits");
  EXPECT_EQ(info_field(*field, "Address"), "requires an object");
  EXPECT_TRUE(has(cpp_->symbol_names(), "InfoBox::get"));
  EXPECT_TRUE(has(cpp_->symbol_names(), "info_count"));
}

TEST_F(CppInterpreterTest, SymbolInfoReportsHooksAndRawOriginalWithoutExecuting) {
  ASSERT_TRUE(cpp_->execute("int info_calls = 0; int info_target(int x) { ++info_calls; return x; } "
                           "int info_caller(int x) { return info_target(x); }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "info_target(int x)", "++info_calls; return 42;", error,
                             "info_caller(int x)")) << error;
  const auto info = cpp_->symbol_info("info_target", error);
  ASSERT_TRUE(info.has_value()) << error;
  EXPECT_EQ(info_field(*info, "Hook"), "%mock in info_caller(int x)");
  const auto original = info_field(*info, "Original IR");
  ASSERT_FALSE(original.empty());
  const auto raw = cpp_->symbol_info(original, error);
  ASSERT_TRUE(raw.has_value()) << error;
  EXPECT_EQ(info_field(*raw, "Kind"), "JIT symbol");
  EXPECT_EQ(info_field(*raw, "Address"), info_field(*info, "Original"));
  EXPECT_EQ(cpp_->evaluate("info_calls"), "(int) 0\n");
  EXPECT_FALSE(cpp_->symbol_info("info_target(1)", error).has_value());
  EXPECT_EQ(cpp_->evaluate("info_calls"), "(int) 0\n");
  ASSERT_TRUE(cpp_->restore("info_target(int x)", error)) << error;
  const auto restored = cpp_->symbol_info("info_target", error);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(info_field(*restored, "Hooks"), "none");
}

TEST_F(CppInterpreterTest, SymbolInfoHandlesUnemittedUndefinedAndUnknownSymbols) {
  ASSERT_TRUE(cpp_->execute("int info_undefined(int); "
                           "struct InfoLazy { int get() const { return 3; } };"));
  std::string error;
  const auto undefined = cpp_->symbol_info("info_undefined", error);
  ASSERT_TRUE(undefined.has_value()) << error;
  EXPECT_EQ(info_field(*undefined, "Definition"), "no");
  EXPECT_NE(info_field(*undefined, "Address").find("unavailable"), std::string::npos);
  auto lazy = cpp_->symbol_info("InfoLazy::get", error);
  ASSERT_TRUE(lazy.has_value()) << error;
  EXPECT_EQ(info_field(*lazy, "Kind"), "method");
  EXPECT_NE(info_field(*lazy, "Address").find("unavailable"), std::string::npos);
  EXPECT_EQ(info_field(*lazy, "Definition"), "yes");
  EXPECT_FALSE(cpp_->symbol_info("no_such_info_symbol", error).has_value());
  EXPECT_EQ(error, "no symbol named 'no_such_info_symbol'");
  EXPECT_EQ(cpp_->evaluate("InfoLazy{}.get()"), "(int) 3\n");
  lazy = cpp_->symbol_info("InfoLazy::get", error);
  ASSERT_TRUE(lazy.has_value()) << error;
  EXPECT_EQ(info_field(*lazy, "Address").find("0x"), 0u);
}

TEST_F(CppInterpreterTest, SymbolInfoUsesAddedSourceLocations) {
  SourceDir dir;
  const auto file = dir.write("symbol_info.cpp", "int info_from_file(int x) { return x+1; }\n");
  ASSERT_TRUE(cpp_->add_source(file).ok);
  std::string error;
  const auto info = cpp_->symbol_info("info_from_file", error);
  ASSERT_TRUE(info.has_value()) << error;
  EXPECT_EQ(info_field(*info, "Location"), std::filesystem::canonical(file).string() + ":1:5");
}

TEST(SymbolFormatting, AlignsFieldsAndSeparatesOverloads) {
  SymbolDescription first;
  first.name = "example";
  first.fields = {{"Kind", "function"}, {"Declaration", "int example(\n int x\n)"},
                   {"Address", "0x123"}};
  const std::string block = "Symbol: example\n"
      "  Kind        : function\n"
      "  Declaration : int example( int x )\n"
      "  Address     : 0x123\n";
  EXPECT_EQ(format_symbols({first}, "example"), block);
  EXPECT_EQ(format_symbols({first, first}, "example"), "2 matches for 'example'\n\n" + block + "\n" + block);
}

TEST(LoadedSymbols, DemanglesAndFormatsSortedDistinctRows) {
  EXPECT_EQ(readable_symbol_name("_ZN4demo3addEii"), "demo::add(int, int)");
  EXPECT_EQ(readable_symbol_name("__ZN4demo3addEii"), "demo::add(int, int)");
  EXPECT_EQ(readable_symbol_name("plain_c_function"), "plain_c_function");
  const LoadedSymbol a{"liba.so", "variable", "demo::count", ""};
  const LoadedSymbol b{"libb.so", "function", "demo::add(int, int)", ""};
  EXPECT_EQ(format_loaded_symbols({b, a, b}),
      "demo::add(int, int)\ndemo::count\n");
  EXPECT_EQ(format_loaded_symbols({}), "");
  EXPECT_NE(format_loaded_symbols({{"bad\nlib", "function", "bad\tsymbol", ""}})
                .find("bad symbol\n"), std::string::npos);
}

TEST(LoadedSymbols, RecognizesSystemLibraryDirectoriesWithoutHidingProjectLibraries) {
  for (const char* path : {"/System/Library/Frameworks/Accelerate.framework/Accelerate",
                           "/usr/lib/libSystem.B.dylib", "/Library/Apple/usr/lib/libRosetta.dylib",
                           "/usr/lib64/libc.so.6", "/lib/x86_64-linux-gnu/libc.so.6",
                           "/lib64/ld-linux-x86-64.so.2", "/opt/homebrew/opt/llvm/lib/libLLVM.dylib",
                           "/usr/local/Cellar/llvm/22/lib/libLLVM.dylib",
                           "/usr/local/opt/llvm/lib/libLLVM.dylib", "linux-vdso.so.1"})
    EXPECT_TRUE(is_system_library(path)) << path;
  for (const char* path : {"/home/user/libpoints.so", "/Users/user/libpoints.dylib",
                           "/opt/project/lib/libpoints.so", "/usr/library/libpoints.so",
                           "/opt/homebrew-project/libpoints.dylib", "<session>"})
    EXPECT_FALSE(is_system_library(path)) << path;
}

TEST_F(CppInterpreterTest, ListsEmittedSessionSymbolsWithoutRunningFunctions) {
  ASSERT_TRUE(cpp_->execute(
      "namespace symbol_listing { int visits=0; "
      "int add(int a,int b) { ++visits; return a+b; } "
      "double add(double a,double b) { return a+b; } "
      "struct Counter { int value() const; }; int Counter::value() const { return 3; } "
      "extern int unresolved; int unavailable(); }"));
  std::string error;
  const auto result = cpp_->symbol_list("^<session>$", "^symbol_listing::", error);
  ASSERT_TRUE(result) << error;
  EXPECT_NE(result->find("symbol_listing::add(int, int)"), std::string::npos) << *result;
  EXPECT_NE(result->find("symbol_listing::add(double, double)"), std::string::npos) << *result;
  EXPECT_NE(result->find("symbol_listing::Counter::value() const"), std::string::npos) << *result;
  EXPECT_EQ(result->find("symbol_listing::unresolved"), std::string::npos) << *result;
  EXPECT_EQ(result->find("symbol_listing::unavailable("), std::string::npos) << *result;
  EXPECT_EQ(result->find("__cpp_repl_"), std::string::npos) << *result;
  const auto helpers = cpp_->symbol_list("^<session>$", "^__", error);
  ASSERT_TRUE(helpers) << error;
  EXPECT_EQ(*helpers, "");
  EXPECT_EQ(cpp_->evaluate("symbol_listing::visits"), "(int) 0\n");
  const auto missing = cpp_->symbol_list("no_such_image_987654321", "", error);
  ASSERT_TRUE(missing) << error;
  EXPECT_EQ(*missing, "");
  EXPECT_FALSE(cpp_->symbol_list("[", "", error));
  EXPECT_NE(error.find("invalid lib regex"), std::string::npos);
  EXPECT_FALSE(cpp_->symbol_list("", "[", error));
  EXPECT_NE(error.find("invalid name regex"), std::string::npos);
  const auto filtered = cpp_->symbol_list("<session>", R"(^symbol_listing::add\(int, int\)$)", error);
  ASSERT_TRUE(filtered) << error;
  EXPECT_EQ(*filtered, "symbol_listing::add(int, int)\n");
  EXPECT_TRUE(error.empty());
}

TEST_F(CppInterpreterTest, SymbolDetailsShowSourceReferencesAndUnresolvedDeclarations) {
  ASSERT_TRUE(cpp_->execute("namespace inspect_refs { int target(int x) { return x+1; } "
      "int caller(int x) { return target(x); } int missing(); "
      "inline int deferred() { return missing(); } }"));
  std::string error;
  const auto target = cpp_->symbol_info("inspect_refs::target", error);
  ASSERT_TRUE(target) << error;
  EXPECT_EQ(info_field(*target, "Defined"), "yes (session definition)");
  EXPECT_EQ(info_field(*target, "Resolved"), "yes");
  EXPECT_EQ(info_field(*target, "Source refs"), "1");
  EXPECT_NE(target->find("in inspect_refs::caller"), std::string::npos) << *target;
  const auto missing = cpp_->symbol_info("inspect_refs::missing", error);
  ASSERT_TRUE(missing) << error;
  EXPECT_EQ(info_field(*missing, "Defined"), "no (declaration only)");
  EXPECT_EQ(info_field(*missing, "Undefined"), "yes (unresolved declaration)");
  EXPECT_EQ(info_field(*missing, "Resolved"), "no (undefined or not emitted)");
  EXPECT_NE(missing->find("in inspect_refs::deferred"), std::string::npos) << *missing;
  EXPECT_EQ(cpp_->evaluate("inspect_refs::caller(2)"), "(int) 3\n");
}

#ifdef CPP_REPL_RUNTIME_SYMBOLS
TEST_F(CppInterpreterTest, SymbolListingHidesSystemExportsUnlessAllIsRequested) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("int listing_local() { return 1; }"));
  const std::string names = R"(^(malloc|listing_local\(\)|hook_library::add\(int, int\))$)";
  std::string error;
  const auto normal = cpp_->symbol_list("", names, error);
  ASSERT_TRUE(normal) << error;
  EXPECT_EQ(*normal, "hook_library::add(int, int)\nlisting_local()\n");
  const auto all = cpp_->symbol_list("", names, error, true);
  ASSERT_TRUE(all) << error;
  EXPECT_EQ(*all, *normal + "malloc\n");
  // A library filter still obeys the default scope; all is an explicit opt-in.
  const std::string system = R"(^(/System/|/usr/lib/|/usr/lib64/|/lib/|/lib64/))";
  EXPECT_EQ(cpp_->symbol_list(system, "^malloc$", error), "");
  EXPECT_EQ(cpp_->symbol_list(system, "^malloc$", error, true), "malloc\n");
  const auto details = cpp_->symbol_info("malloc", error);
  ASSERT_TRUE(details) << error;
  EXPECT_NE(details->find("native export"), std::string::npos) << *details;
}

TEST_F(CppInterpreterTest, LibraryDetailsIncludePathsDependenciesAndAbiMetadata) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  std::string error;
  const auto names = library_list("hook_test_library", error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(names, std::filesystem::path(HOOK_TEST_LIBRARY).filename().string() + "\n");
  EXPECT_EQ(names.find('/'), std::string::npos);
  const auto info = library_info("hook_test_library", error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_NE(info.find("Library: "), std::string::npos) << info;
  EXPECT_EQ(info_field(info, "Path"), HOOK_TEST_LIBRARY);
  EXPECT_NE(info.find("Version"), std::string::npos) << info;
  EXPECT_NE(info.find("Dependency"), std::string::npos) << info;
#ifdef __linux__
  EXPECT_NE(info.find("GLIBC req"), std::string::npos) << info;
  const auto libc = library_info("libc.so.6", error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_NE(libc.find("GLIBC_2."), std::string::npos) << libc;
  EXPECT_NE(libc.find("running process"), std::string::npos) << libc;
#else
  EXPECT_NE(info.find("Current"), std::string::npos) << info;
  EXPECT_NE(info.find("Compatible"), std::string::npos) << info;
  EXPECT_NE(info.find("UUID"), std::string::npos) << info;
#endif
  EXPECT_TRUE(library_list("unlikely_library_987654321", error).empty());
  EXPECT_TRUE(error.empty());
  library_info("unlikely_library_987654321", error);
  EXPECT_NE(error.find("no loaded library"), std::string::npos);
  library_list("[", error);
  EXPECT_NE(error.find("invalid library regex"), std::string::npos);
}

TEST_F(CppInterpreterTest, LibraryListingHidesSystemLibrariesUnlessAllIsRequested) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  std::string error;
  const auto project = std::filesystem::path(HOOK_TEST_LIBRARY).filename().string() + "\n";
  EXPECT_EQ(library_list("hook_test_library", error), project);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(library_list("hook_test_library", error, true), project);
#ifdef __linux__
  const std::string system = "libc.so.6";
  const std::string pattern = R"(^libc\.so\.6$)";
#else
  const std::string system = "libSystem.B.dylib";
  const std::string pattern = R"(^libSystem\.B\.dylib$)";
#endif
  EXPECT_EQ(library_list(pattern, error), "");
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(library_list(pattern, error, true), system + '\n');
  EXPECT_TRUE(error.empty()) << error;
  const auto normal = library_list("", error);
  EXPECT_NE(normal.find(project), std::string::npos) << normal;
  EXPECT_EQ(normal.find(system + '\n'), std::string::npos) << normal;
  const auto all = library_list("", error, true);
  EXPECT_NE(all.find(project), std::string::npos) << all;
  EXPECT_NE(all.find(system + '\n'), std::string::npos) << all;
  EXPECT_NE(library_info(system, error).find("Library: " + system), std::string::npos);
  EXPECT_TRUE(error.empty()) << error;
  library_list("[", error, true);
  EXPECT_NE(error.find("invalid library regex"), std::string::npos);
}

TEST_F(CppInterpreterTest, NativeSymbolDetailsWorkWithoutHeadersAndShowCallSites) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  std::string error;
  const auto info = cpp_->symbol_info("hook_library::NativePoint::inc(int)", error);
  ASSERT_TRUE(info) << error;
  EXPECT_NE(info->find("Symbol: hook_library::NativePoint::inc(int)"), std::string::npos) << *info;
  EXPECT_EQ(info_field(*info, "Defined"), "yes (native export)");
  EXPECT_EQ(info_field(*info, "Resolved"), "yes");
  EXPECT_EQ(info_field(*info, "Path"), HOOK_TEST_LIBRARY);
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  EXPECT_NE(info->find("in hook_library::NativePoint::run(int)"), std::string::npos) << *info;
#endif
  const auto raw = cpp_->symbol_info("_ZN12hook_library11NativePoint3incEi", error);
  ASSERT_TRUE(raw) << error;
  EXPECT_NE(raw->find("hook_library::NativePoint::inc(int)"), std::string::npos) << *raw;
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  const auto method = cpp_->symbol_info("hook_library::NativePoint::inc(int)", error);
  ASSERT_TRUE(method) << error;
  EXPECT_EQ(info_field(*method, "Kind"), "method");
#ifdef __linux__
  const auto missing = cpp_->symbol_info("hook_native_weak_missing", error);
  ASSERT_TRUE(missing) << error;
  EXPECT_EQ(info_field(*missing, "Defined"), "no (undefined import)");
  EXPECT_EQ(info_field(*missing, "Undefined"), "yes (native import)");
  EXPECT_EQ(info_field(*missing, "Resolved"), "no (undefined or not emitted)");
  EXPECT_NE(missing->find("(undefined, unresolved)"), std::string::npos) << *missing;
  EXPECT_EQ(cpp_->evaluate("hook_library::native_optional()"), "(int) -1\n");
#endif
}

TEST_F(CppInterpreterTest, ListsLibraryExportsWithoutHeadersAndRefinesKindsWithDeclarations) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  std::string error;
  const auto result = cpp_->symbol_list("hook_test_library",
      R"(^hook_library::(add|NativePoint::inc|symbol_counter)(\(|$))", error);
  ASSERT_TRUE(result) << error;
  EXPECT_NE(result->find("hook_library::add(int, int)"), std::string::npos) << *result;
  EXPECT_NE(result->find("hook_library::add(double, double)"), std::string::npos) << *result;
  EXPECT_NE(result->find("hook_library::NativePoint::inc(int)"), std::string::npos) << *result;
  EXPECT_NE(result->find("hook_library::symbol_counter"), std::string::npos) << *result;
  EXPECT_EQ(std::count(result->begin(), result->end(), '\n'), 4);
  EXPECT_EQ(result->find("_ZN12hook_library"), std::string::npos);
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  const auto method = cpp_->symbol_list("hook_test_library", "^hook_library::NativePoint::inc", error);
  ASSERT_TRUE(method) << error;
  EXPECT_EQ(*method, "hook_library::NativePoint::inc(int)\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::symbol_counter"), "(int) 7\n");
}
#endif

TEST_F(CppInterpreterTest, MockAffectsEarlierCallersAndFunctionPointers) {
  ASSERT_TRUE(cpp_->execute("int mock_twice(int x) { return x * 2; }"));
  ASSERT_TRUE(cpp_->execute("int mock_caller() { return mock_twice(3); }"));
  ASSERT_TRUE(cpp_->execute("int (*mock_pointer)(int) = &mock_twice;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int mock_twice(int x)", "return x + 10;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("mock_caller()"), "(int) 13\n");
  EXPECT_EQ(cpp_->evaluate("mock_pointer(4)"), "(int) 14\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int mock_twice(int x)", "return x + 20;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("mock_caller()"), "(int) 23\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int mock_twice(int x)", "", error)) << error;
  EXPECT_EQ(cpp_->evaluate("mock_caller()"), "(int) 6\n");
}

TEST_F(CppInterpreterTest, HooksRunInOrderAndKeepTheirOwnParameterNames) {
  ASSERT_TRUE(cpp_->execute("int hook_order = 0; int hook_value(int x) { hook_order = hook_order*10+2; return x*2; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "int hook_value(int before)",
                           "hook_order = hook_order*10+1; before += 2;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "int hook_value(int after)",
                           "hook_order = hook_order*10+3; result += after;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_value(3)"), "(int) 15\n");
  EXPECT_EQ(cpp_->evaluate("hook_order"), "(int) 123\n");
  ASSERT_TRUE(cpp_->execute("hook_order = 0;"));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int hook_value(int replacement)",
                           "hook_order = hook_order*10+4; return replacement+10;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_value(3)"), "(int) 20\n");
  EXPECT_EQ(cpp_->evaluate("hook_order"), "(int) 143\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "int hook_value(int x)", "", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "int hook_value(int x)", "", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_value(3)"), "(int) 13\n");
}

TEST_F(CppInterpreterTest, RestoreClearsAllHooksForOnlyTheSelectedOverload) {
  ASSERT_TRUE(cpp_->execute("int restore_events = 0; "
      "struct RestoreCounter { int value = 10; "
      "int add(int x) { return value+x; } double add(double x) { return value+x; } }; "
      "RestoreCounter restore_counter; "
      "int restore_caller() { return restore_counter.add(3); }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "RestoreCounter::add(int x)",
                           "++restore_events; x+=1;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "RestoreCounter::add(int x)",
                           "++restore_events; return self->value+x*2;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "RestoreCounter::add(int x)",
                           "++restore_events; result*=2;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "RestoreCounter::add(double x)",
                           "return 99.0;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("restore_caller()"), "(int) 36\n");
  EXPECT_EQ(cpp_->evaluate("restore_events"), "(int) 3\n");
  EXPECT_FALSE(cpp_->restore("RestoreCounter::add(float x)", error));
  EXPECT_EQ(cpp_->evaluate("restore_caller()"), "(int) 36\n");
  ASSERT_TRUE(cpp_->restore("int RestoreCounter::add(int result)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("restore_caller()"), "(int) 13\n");
  EXPECT_EQ(cpp_->evaluate("restore_events"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("restore_counter.add(3.0)"), "(double) 99.0\n");
  ASSERT_TRUE(cpp_->restore("RestoreCounter::add(int x)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("restore_caller()"), "(int) 13\n");
  // New hooks must not bring back any old pre, mock or post body.
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "RestoreCounter::add(int x)",
                           "result+=1;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("restore_caller()"), "(int) 14\n");
  EXPECT_EQ(cpp_->evaluate("restore_events"), "(int) 6\n");
}

TEST_F(CppInterpreterTest, RestoreSupportsFunctionsAndUnhookedMethods) {
  ASSERT_TRUE(cpp_->execute("int restore_function(int x) { return x+1; } "
      "struct RestoreFresh { int get() const { return 7; } };"));
  std::string error;
  ASSERT_TRUE(cpp_->restore("RestoreFresh::get() const", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "restore_function(int x)", "return 42;", error)) << error;
  ASSERT_TRUE(cpp_->restore("restore_function(int x)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("restore_function(2)"), "(int) 3\n");
  EXPECT_FALSE(cpp_->restore("not_a_function", error));
  EXPECT_FALSE(cpp_->restore("missing_function(int x)", error));
  EXPECT_FALSE(cpp_->restore("restore_function(int x) cpp``", error));
  EXPECT_EQ(cpp_->evaluate("restore_function(2)"), "(int) 3\n");
}

TEST_F(CppInterpreterTest, BareRestoreClearsEveryTargetAndAllowsNewHooks) {
  std::string error;
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  ASSERT_TRUE(cpp_->execute("int all_restore_visits = 0; "
      "int all_restore_f(int x) { return x+1; } "
      "double all_restore_f(double x) { return x+2; } "
      "struct AllRestore { int get() const { return 7; } }; AllRestore all_restore_obj; "
      "int all_restore_caller() { return all_restore_f(3)+all_restore_obj.get(); }"));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "all_restore_f(int x)",
                           "++all_restore_visits; x+=1;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "all_restore_f(int x)",
                           "return x*3;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "all_restore_f(int x)",
                           "++all_restore_visits; result+=1;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "all_restore_f(double x)",
                           "result*=2;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "AllRestore::get() const",
                           "return 100;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("all_restore_caller()"), "(int) 113\n");
  EXPECT_EQ(cpp_->evaluate("all_restore_f(2.0)"), "(double) 8.0\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("all_restore_caller()"), "(int) 11\n");
  EXPECT_EQ(cpp_->evaluate("all_restore_f(2.0)"), "(double) 4.0\n");
  EXPECT_EQ(cpp_->evaluate("all_restore_visits"), "(int) 2\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "all_restore_f(int x)",
                           "x+=1;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("all_restore_caller()"), "(int) 12\n");
  EXPECT_EQ(cpp_->evaluate("all_restore_visits"), "(int) 2\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
}

TEST_F(CppInterpreterTest, ListsOnlyActiveHooksWithoutExecutingThem) {
  std::string error;
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  ASSERT_TRUE(cpp_->execute("int listing_runs = 0; "
      "int listing_f(int x) { return x; } double listing_f(double x) { return x; } "
      "struct ListingThing { int get() const { return 7; } };"));
  EXPECT_TRUE(cpp_->active_hooks().empty());
  const std::string mock_code = "\n  // Preserve a ` and indentation.\n  ++listing_runs; return x+10;\n";
  const std::string pre_code = "++listing_runs; before+=1;";
  const std::string post_code = "\n// Keep original this syntax.\n++listing_runs;\n"
                                "result += this != nullptr ? 1 : 0;\n";
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int listing_f(int x)",
                           mock_code, error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "listing_f(double before)",
                           pre_code, error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "ListingThing::get() const",
                           post_code, error)) << error;
  auto entries = cpp_->active_hooks();
  auto has_entry = [&](HookKind kind, const std::string& signature) {
    return std::any_of(entries.begin(), entries.end(), [&](const ActiveHook& entry) {
      return entry.kind == kind && entry.signature == signature;
    });
  };
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_TRUE(has_entry(HookKind::Mock, "int listing_f(int x)"));
  EXPECT_TRUE(has_entry(HookKind::Pre, "listing_f(double before)"));
  EXPECT_TRUE(has_entry(HookKind::Post, "ListingThing::get() const"));
  auto has_body = [&](HookKind kind, const std::string& body) {
    return std::any_of(entries.begin(), entries.end(), [&](const ActiveHook& entry) {
      return entry.kind == kind && entry.body == body;
    });
  };
  EXPECT_TRUE(has_body(HookKind::Mock, mock_code));
  EXPECT_TRUE(has_body(HookKind::Pre, pre_code));
  EXPECT_TRUE(has_body(HookKind::Post, post_code));
  EXPECT_EQ(cpp_->evaluate("listing_runs"), "(int) 0\n");
  EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end(), [](const ActiveHook& a, const ActiveHook& b) {
    return a.signature < b.signature || (a.signature == b.signature && a.kind < b.kind);
  }));
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "listing_f(int renamed)",
                            "return missing_listing_name;", error));
  entries = cpp_->active_hooks();
  EXPECT_TRUE(has_entry(HookKind::Mock, "int listing_f(int x)"));
  EXPECT_TRUE(has_body(HookKind::Mock, mock_code));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "listing_f(int replacement)",
                           "return replacement+20;", error)) << error;
  entries = cpp_->active_hooks();
  EXPECT_EQ(entries.size(), 3u);
  EXPECT_TRUE(has_entry(HookKind::Mock, "listing_f(int replacement)"));
  EXPECT_FALSE(has_entry(HookKind::Mock, "int listing_f(int x)"));
  EXPECT_TRUE(has_body(HookKind::Mock, "return replacement+20;"));
  EXPECT_FALSE(has_body(HookKind::Mock, mock_code));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "listing_f(double x)", "", error)) << error;
  EXPECT_EQ(cpp_->active_hooks().size(), 2u);
  ASSERT_TRUE(cpp_->restore("listing_f(int x)", error)) << error;
  entries = cpp_->active_hooks();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_TRUE(has_entry(HookKind::Post, "ListingThing::get() const"));
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_TRUE(cpp_->active_hooks().empty());
}

TEST_F(CppInterpreterTest, CallerScopedMockOnlyChangesTheSelectedMethod) {
  ASSERT_TRUE(cpp_->execute("struct ScopedCalc { int base = 10; "
      "int add(int x) { return base+x; } "
      "int inc(int n) { return add(n); } "
      "int double_it(int n) { return add(n)*2; } }; ScopedCalc scoped_calc;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "ScopedCalc::add(int x)",
      "return this->base + x*10;", error, "ScopedCalc::inc(int n)")) << error;
  EXPECT_EQ(cpp_->evaluate("scoped_calc.inc(3)"), "(int) 40\n");
  EXPECT_EQ(cpp_->evaluate("scoped_calc.double_it(3)"), "(int) 26\n");
  EXPECT_EQ(cpp_->evaluate("scoped_calc.add(3)"), "(int) 13\n");
  ASSERT_TRUE(cpp_->restore("ScopedCalc::add(int x)", error, "ScopedCalc::inc(int n)")) << error;
  EXPECT_EQ(cpp_->evaluate("scoped_calc.inc(3)"), "(int) 13\n");
}

TEST_F(CppInterpreterTest, PrivateCallersCanBeMaterializedWithoutChangingUserAccess) {
  ASSERT_TRUE(cpp_->execute("int private_caller_hits = 0; "
      "int private_caller_add(int a, int b) { return a+b; } "
      "class PrivateCaller { int x = 0; void inc(int s) { x = private_caller_add(x,s); } "
      "public: int run(int s) { inc(s); return x; } "
      "int double_it(int s) { return private_caller_add(s,s); } }; PrivateCaller private_caller;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "private_caller_add(int, int)",
      "++private_caller_hits;", error, "PrivateCaller::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("private_caller.run(3)"), "(int) 3\n");
  EXPECT_EQ(cpp_->evaluate("private_caller.double_it(3)"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("private_caller_hits"), "(int) 1\n");
  EXPECT_FALSE(cpp_->execute("private_caller.inc(1);"));
  EXPECT_FALSE(cpp_->execute("auto private_member_pointer = &PrivateCaller::inc;"));
  ASSERT_TRUE(cpp_->restore("private_caller_add(int, int)", error, "PrivateCaller::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("private_caller.run(2)"), "(int) 5\n");
  EXPECT_EQ(cpp_->evaluate("private_caller_hits"), "(int) 1\n");
}

TEST_F(CppInterpreterTest, LibraryFunctionsSupportHooksAndPreserveNativeCalls) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  ASSERT_TRUE(cpp_->execute("int library_visits = 0; "
      "int library_caller(int n) { return hook_library::add(n,1); } "
      "int (*library_pointer)(int,int) = &hook_library::add;"));
  EXPECT_EQ(cpp_->evaluate("library_caller(3)"), "(int) 4\n");
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "hook_library::add(int a,int b)",
      "++library_visits; ++a;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "hook_library::add(int,int)",
      "result *= 2;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("library_caller(3)"), "(int) 10\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int a,int b)",
      "return a+b+100;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("library_caller(3)"), "(int) 210\n");
  EXPECT_EQ(cpp_->evaluate("library_pointer(3,1)"), "(int) 210\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3,1)"), "(int) 210\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3.0,1.0)"), "(double) 4.0\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::native_inc(3)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("library_visits"), "(int) 4\n");
  const auto info = cpp_->symbol_info("hook_library::add", error);
  ASSERT_TRUE(info.has_value()) << error;
  EXPECT_NE(info->find("Routing IR"), std::string::npos) << *info;
  EXPECT_NE(info->find("__cpp_repl_external_"), std::string::npos) << *info;
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return missing_library_mock_name;", error));
  EXPECT_EQ(cpp_->evaluate("library_caller(3)"), "(int) 210\n");
  ASSERT_TRUE(cpp_->restore("hook_library::add(int,int)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("library_caller(3)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("library_pointer(3,1)"), "(int) 4\n");
}

TEST_F(CppInterpreterTest, LibraryHooksCanBeInstalledBeforeTheFirstCall) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(double a,double b)",
      "return a*b;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3.0,2.0)"), "(double) 6.0\n");
  ASSERT_TRUE(cpp_->restore("hook_library::add(double,double)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3.0,2.0)"), "(double) 5.0\n");
}

TEST_F(CppInterpreterTest, PrivateLibraryTargetsSupportCallerScopedHooks) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  ASSERT_TRUE(cpp_->execute("hook_library::Point library_point; int private_library_visits=0;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "hook_library::Point::add(int n) const",
      "++private_library_visits; ++n;", error, "hook_library::Point::inc(int) const")) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "hook_library::Point::add(int) const",
      "result *= 2;", error, "hook_library::Point::inc(int) const")) << error;
  EXPECT_EQ(cpp_->evaluate("library_point.inc(3)"), "(int) 10\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::Point::add(int n) const",
      "return n+100;", error, "hook_library::Point::inc(int) const")) << error;
  EXPECT_EQ(cpp_->evaluate("library_point.inc(3)"), "(int) 208\n");
  EXPECT_EQ(cpp_->evaluate("library_point.double_it(3)"), "(int) 8\n");
  EXPECT_EQ(cpp_->evaluate("private_library_visits"), "(int) 2\n");
  EXPECT_FALSE(cpp_->execute("library_point.add(3);"));
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 7;", error, "hook_library::native_inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::native_inc(3)"), "(int) 7\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3,1)"), "(int) 4\n");
#else
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 7;", error, "hook_library::native_inc(int)"));
  EXPECT_NE(error.find("runtime patch backend"), std::string::npos) << error;
#endif
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("library_point.inc(3)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("private_library_visits"), "(int) 2\n");
}

#ifdef CPP_REPL_NATIVE_CALL_PATCHES
TEST_F(CppInterpreterTest, PrecompiledPrivateCallerSupportsAllHooksWithoutRebuilding) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  ASSERT_TRUE(cpp_->execute("hook_library::NativePoint native_point; int native_visits=0; int native_posts=0;"));
  std::ifstream before_file(HOOK_TEST_LIBRARY, std::ios::binary);
  const std::string before_disk((std::istreambuf_iterator<char>(before_file)), {});
  ASSERT_FALSE(before_disk.empty());
  void* handle = dlopen(HOOK_TEST_LIBRARY, RTLD_NOW);
  ASSERT_NE(handle, nullptr);
  auto* code = static_cast<const char*>(dlsym(handle, "_ZN12hook_library11NativePoint3incEi"));
  ASSERT_NE(code, nullptr);
  const std::string before_code(code, 32);
#ifdef __linux__
  auto permissions = [code] {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
      std::istringstream fields(line);
      std::uintptr_t begin = 0, end = 0;
      char separator = 0;
      std::string protection;
      fields >> std::hex >> begin >> separator >> end >> protection;
      const auto address = reinterpret_cast<std::uintptr_t>(code);
      if (separator == '-' && address >= begin && address < end) return protection;
    }
    return std::string{};
  };
  const auto before_permissions = permissions();
  ASSERT_NE(before_permissions.find('x'), std::string::npos);
#endif
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "hook_library::add(int a,int b)",
      "++native_visits; ++a;", error, "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_NE(before_code, std::string(code, 32));
#ifdef __linux__
  EXPECT_EQ(permissions(), before_permissions);
#endif
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "hook_library::add(int,int)",
      "++native_posts; result += 10;", error, "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 28\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int a,int b)",
      "return a+b+100;", error, "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 228\n");
  EXPECT_EQ(cpp_->evaluate("native_point.double_it(3)"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::add(3,1)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("native_visits"), "(int) 4\n");
  EXPECT_FALSE(cpp_->execute("native_point.inc(3);"));
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return nonexistent_native_name;", error, "hook_library::NativePoint::inc(int)"));
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 228\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int a,int b)",
      "throw a;", error, "hook_library::NativePoint::inc(int)")) << error;
  ASSERT_TRUE(cpp_->execute("int native_caught=0; try { native_point.run(3); } "
      "catch (int x) { native_caught=x; }"));
  EXPECT_EQ(cpp_->evaluate("native_caught"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("native_posts"), "(int) 6\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 99;", error, "hook_library::NativePoint::double_it(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.double_it(3)"), "(int) 99\n");
  ASSERT_TRUE(cpp_->restore("hook_library::add(int,int)", error,
      "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_EQ(before_code, std::string(code, 32));
#ifdef __linux__
  EXPECT_EQ(permissions(), before_permissions);
#endif
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("native_point.double_it(3)"), "(int) 99\n");
  ASSERT_TRUE(cpp_->restore("hook_library::add(int,int)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.double_it(3)"), "(int) 6\n");
  // Reuse a restored edge, then remove its last active hook with an empty body.
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 8;", error, "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 8\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "", error, "hook_library::NativePoint::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("native_point.run(3)"), "(int) 6\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  std::ifstream after_file(HOOK_TEST_LIBRARY, std::ios::binary);
  const std::string after_disk((std::istreambuf_iterator<char>(after_file)), {});
  EXPECT_EQ(before_disk, after_disk);
  dlclose(handle);
}

TEST_F(CppInterpreterTest, NativeCallsFromTheHostRestoreWhenTheSessionEnds) {
  void* handle = dlopen(HOOK_TEST_LIBRARY, RTLD_NOW);
  ASSERT_NE(handle, nullptr);
  auto* address = dlsym(handle, "_ZN12hook_library10native_incEi");
  ASSERT_NE(address, nullptr);
  const auto native = reinterpret_cast<int (*)(int)>(address);
  EXPECT_EQ(native(3), 4);
  std::string error;
  auto session = CppInterpreter::create({}, error);
  ASSERT_NE(session, nullptr) << error;
  ASSERT_TRUE(session->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(session->execute("#include \"" HOOK_TEST_HEADER "\""));
  ASSERT_TRUE(session->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 81;", error, "hook_library::native_inc(int)")) << error;
  EXPECT_EQ(native(3), 81);
  session.reset();
  EXPECT_EQ(native(3), 4);
  dlclose(handle);
}

TEST_F(CppInterpreterTest, NativeFiltersRejectMissingCallsWithoutChangingOtherHooks) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 61;", error, "hook_library::native_inc(int)")) << error;
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "hook_library::add(int,int)",
      "return 81;", error, "hook_library::native_no_add(int)"));
  EXPECT_NE(error.find("native caller has no"), std::string::npos) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::native_inc(3)"), "(int) 61\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::native_no_add(3)"), "(int) 4\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::native_inc(3)"), "(int) 4\n");
}

TEST_F(CppInterpreterTest, NativeSymbolsWinOverInlineHeaderBodies) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  ASSERT_TRUE(cpp_->execute("hook_library::InlineNativePoint inline_native_point;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::inline_add(int a,int b)",
      "return a+b+100;", error, "hook_library::InlineNativePoint::inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("inline_native_point.run(3)"), "(int) 104\n");
  EXPECT_EQ(cpp_->evaluate("inline_native_point.double_it(3)"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::inline_add(3,1)"), "(int) 4\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("inline_native_point.run(3)"), "(int) 4\n");
}

#ifdef __linux__
TEST_F(CppInterpreterTest, NativeGotCallsOnlyChangeTheSelectedCaller) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\""));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::got_add(int a,int b)",
      "return a+b+100;", error, "hook_library::got_inc(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::got_inc(3)"), "(int) 105\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::got_double(3)"), "(int) 6\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::got_add(3,1)"), "(int) 4\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::got_inc(3)"), "(int) 5\n");
  // Also cover a GOT tail jump, independently of the non-tail call above.
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_library::got_add(int a,int b)",
      "return a+b+100;", error, "hook_library::got_double(int)")) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::got_double(3)"), "(int) 106\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::got_inc(3)"), "(int) 5\n");
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_library::got_double(3)"), "(int) 6\n");
}
#endif
#endif

TEST_F(CppInterpreterTest, PrivateTargetsAndProtectedCallersKeepHookAccessChecks) {
  ASSERT_TRUE(cpp_->execute("class HiddenTarget { int value = 5; "
      "int add(int n) { return value+n; } "
      "public: int run(int n) { return add(n); } }; HiddenTarget hidden_target; "
      "int protected_add(int a, int b) { return a+b; } "
      "class ProtectedCaller { protected: int inc(int n) { return protected_add(n,1); } "
      "public: int run(int n) { return inc(n); } }; ProtectedCaller protected_caller;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "HiddenTarget::add(int n)", "return n+10;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hidden_target.run(3)"), "(int) 13\n");
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "HiddenTarget::add(int n)",
                            "return this->value+n;", error));
  EXPECT_EQ(cpp_->evaluate("hidden_target.run(3)"), "(int) 13\n");
  EXPECT_FALSE(cpp_->execute("hidden_target.add(3);"));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "protected_add(int a, int b)", "return 99;", error,
                            "ProtectedCaller::inc(int n)")) << error;
  EXPECT_EQ(cpp_->evaluate("protected_caller.run(3)"), "(int) 99\n");
  EXPECT_EQ(cpp_->evaluate("protected_add(3,1)"), "(int) 4\n");
  EXPECT_FALSE(cpp_->execute("protected_caller.inc(3);"));
}

TEST_F(CppInterpreterTest, FailedEmissionKeepsAccessChecksAndSessionUsable) {
  ASSERT_TRUE(cpp_->execute("int failed_private_dependency(); "
      "int failed_private_add(int a, int b) { return a+b; } "
      "class FailedPrivateCaller { int inc(int n) { return failed_private_add(n,failed_private_dependency()); } "
      "}; FailedPrivateCaller failed_private_caller;"));
  std::string error;
  EXPECT_FALSE(cpp_->set_hook(HookKind::Pre, "failed_private_add(int, int)", ";", error,
                              "FailedPrivateCaller::inc(int)"));
  EXPECT_NE(error.find("undefined symbol in hook"), std::string::npos) << error;
  EXPECT_FALSE(cpp_->execute("failed_private_caller.inc(1);"));
  EXPECT_FALSE(cpp_->execute("auto failed_private_pointer = &FailedPrivateCaller::inc;"));
  EXPECT_EQ(cpp_->evaluate("failed_private_add(2,3)"), "(int) 5\n");
}

TEST_F(CppInterpreterTest, CallerFilterMatchesDirectCallsAndCallerOverloads) {
  ASSERT_TRUE(cpp_->execute("int edge_add(int x) { return x+1; } "
      "int edge_helper(int x) { return edge_add(x); } "
      "int edge_inc(int n) { return edge_add(n)+edge_add(n)+edge_helper(n); } "
      "int edge_inc(double n) { return edge_add((int)n); } "
      "int edge_double(int n) { return edge_add(n)*2; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "edge_add(int x)", "return 100;", error,
      "edge_inc(int n)")) << error;
  EXPECT_EQ(cpp_->evaluate("edge_inc(3)"), "(int) 204\n");
  EXPECT_EQ(cpp_->evaluate("edge_inc(3.0)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("edge_double(3)"), "(int) 8\n");
  EXPECT_EQ(cpp_->evaluate("edge_add(3)"), "(int) 4\n");
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "edge_add(int x)", "return 9;", error,
      "edge_double(double n)"));
  EXPECT_EQ(cpp_->evaluate("edge_inc(3)"), "(int) 204\n");
  ASSERT_TRUE(cpp_->restore("edge_add(int x)", error)) << error;
  EXPECT_EQ(cpp_->evaluate("edge_inc(3)"), "(int) 12\n");
}

TEST_F(CppInterpreterTest, ScopedHooksAreListedRestoredAndFallBackToGlobalHooks) {
  std::string error;
  ASSERT_TRUE(cpp_->restore("", error));
  ASSERT_TRUE(cpp_->execute("int scope_target(int x) { return x+1; } "
      "int scope_a(int x) { return scope_target(x); } "
      "int scope_b(int x) { return scope_target(x); }"));
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "scope_target(int x)", "return 20;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "scope_target(int before)", "before+=1;", error, "scope_a(int x)")) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "scope_target(int x)", "return x*10;", error, "scope_a(int x)")) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "scope_target(int after)", "result+=after;", error, "scope_a(int x)")) << error;
  EXPECT_EQ(cpp_->evaluate("scope_a(3)"), "(int) 44\n");
  EXPECT_EQ(cpp_->evaluate("scope_b(3)"), "(int) 20\n");
  auto entries = cpp_->active_hooks();
  ASSERT_EQ(entries.size(), 4u);
  EXPECT_EQ(std::count_if(entries.begin(), entries.end(), [](const ActiveHook& entry) {
    return entry.caller == "scope_a(int x)";
  }), 3);
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "scope_target(int x)", "return bad_scoped_code;", error, "scope_a(int x)"));
  EXPECT_EQ(cpp_->evaluate("scope_a(3)"), "(int) 44\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "scope_target(int x)", "", error, "scope_a(int x)")) << error;
  EXPECT_EQ(cpp_->evaluate("scope_a(3)"), "(int) 9\n");  // scoped pre/original/post
  ASSERT_TRUE(cpp_->restore("scope_target(int x)", error, "scope_a(int x)")) << error;
  EXPECT_EQ(cpp_->evaluate("scope_a(3)"), "(int) 20\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "scope_target(int x)", "return 99;", error, "scope_b(int x)")) << error;
  ASSERT_TRUE(cpp_->restore("", error)) << error;
  EXPECT_TRUE(cpp_->active_hooks().empty());
  EXPECT_EQ(cpp_->evaluate("scope_a(3)"), "(int) 4\n");
  EXPECT_EQ(cpp_->evaluate("scope_b(3)"), "(int) 4\n");
}

TEST_F(CppInterpreterTest, CallerFiltersRejectIndirectAndTransitiveCalls) {
  ASSERT_TRUE(cpp_->execute("int indirect_add(int x) { return x+1; } "
      "int (*indirect_ptr)(int) = &indirect_add; "
      "int indirect_inc(int x) { return indirect_ptr(x); } "
      "int transitive_inc(int x) { return indirect_inc(x); }"));
  std::string error;
  for (const char* caller : {"indirect_inc(int x)", "transitive_inc(int x)"}) {
    EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "indirect_add(int x)", "return 99;", error, caller));
    EXPECT_NE(error.find("no supported direct call"), std::string::npos) << error;
  }
  EXPECT_EQ(cpp_->evaluate("transitive_inc(3)"), "(int) 4\n");
}

TEST(FunctionHooks, ScopedMocksWorkInOptimizedSessions) {
  std::string error;
  auto cpp = CppInterpreter::create({"-O2"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int scoped_opt_add(int x) { return x+1; } "
      "int scoped_opt_inc(int x) { return scoped_opt_add(x); } "
      "int scoped_opt_double(int x) { return scoped_opt_add(x)*2; }"));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "scoped_opt_add(int x)", "return 99;", error,
      "scoped_opt_inc(int x)")) << error;
  EXPECT_EQ(cpp->evaluate("scoped_opt_inc(3)"), "(int) 99\n");
  EXPECT_EQ(cpp->evaluate("scoped_opt_double(3)"), "(int) 8\n");
}

TEST_F(CppInterpreterTest, HooksResolveNamespaceOverloadsAndAliases) {
  ASSERT_TRUE(cpp_->execute("namespace hooked { using Number = int; "
                           "int f(int x) { return x+1; } double f(double x) { return x+2; } }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hooked::Number ::hooked::f(hooked::Number x)",
                           "return x+20;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hooked::f(2)"), "(int) 22\n");
  EXPECT_EQ(cpp_->evaluate("hooked::f(2.0)"), "(double) 4.0\n");
}

TEST_F(CppInterpreterTest, HooksMaterializeInlineMethodsAndRespectConstOverloads) {
  ASSERT_TRUE(cpp_->execute("struct HookedPoint { int x; int get(int n) { return x+n; } "
                           "int get(int n) const { return x-n; } static int count() { return 1; } };"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int HookedPoint::get(int n) const",
                           "return this->x * n;", error)) << error;
  ASSERT_TRUE(cpp_->execute("HookedPoint hook_point{7}; const HookedPoint hook_const{7};"));
  EXPECT_EQ(cpp_->evaluate("hook_point.get(3)"), "(int) 10\n");
  EXPECT_EQ(cpp_->evaluate("hook_const.get(3)"), "(int) 21\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "int HookedPoint::get(int n)",
                           "self->x += 2; n += 1;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_point.get(3)"), "(int) 13\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int HookedPoint::count()", "return 9;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("HookedPoint::count()"), "(int) 9\n");
}

TEST_F(CppInterpreterTest, HooksHandleVoidAndReferenceResults) {
  ASSERT_TRUE(cpp_->execute("int ref_storage = 1; int& ref_value() { return ref_storage; } "
                           "void hook_write(int& x) { x += 2; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "void hook_write(int& x)", "x += 1;", error)) << error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "void hook_write(int& x)", "x *= 2;", error)) << error;
  ASSERT_TRUE(cpp_->execute("hook_write(ref_storage);"));
  EXPECT_EQ(cpp_->evaluate("ref_storage"), "(int) 8\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "int& ref_value()", "result += 5;", error)) << error;
  ASSERT_TRUE(cpp_->execute("ref_value() = 30;"));
  EXPECT_EQ(cpp_->evaluate("ref_storage"), "(int) 30\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "void hook_write(int& x)", "x = 4; return;", error)) << error;
  ASSERT_TRUE(cpp_->execute("hook_write(ref_storage);"));
  EXPECT_EQ(cpp_->evaluate("ref_storage"), "(int) 8\n");
}

TEST_F(CppInterpreterTest, FailedHookUpdatesPreserveWorkingMockAndSession) {
  ASSERT_TRUE(cpp_->execute("int stable_mock(int x) { return x; } int hook_missing(); extern int hook_missing_global;"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int stable_mock(int x)", "return x+10;", error)) << error;
  for (const char* body : {"return ;", "return no_such_hook_name;", "return hook_missing();",
                           "return hook_missing_global;", "int no_return = 1;"}) {
    EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "int stable_mock(int x)", body, error)) << body;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(cpp_->evaluate("stable_mock(3)"), "(int) 13\n") << body;
  }
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "double stable_mock(int x)", "return 3;", error));
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "int nonexistent(int x)", "return 3;", error));
  EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, "stable_mock x", "return 3;", error));
  EXPECT_EQ(cpp_->evaluate("2+3"), "(int) 5\n");
}

TEST_F(CppInterpreterTest, HooksApplyToAddedSourcesAndVirtualCalls) {
  SourceDir dir;
  const std::string file = dir.write("hooks.cpp", "struct HookBase { virtual int value() { return 1; } };\n"
      "struct HookDerived : HookBase { int value() override { return 2; } };\n"
      "int hook_virtual(HookBase* b) { return b->value(); }\n"
      "int hook_file(int x) { return x+1; }\n");
  ASSERT_TRUE(cpp_->add_source(file).ok);
  ASSERT_TRUE(cpp_->execute("HookDerived hook_derived;"));
  EXPECT_EQ(cpp_->evaluate("hook_virtual(&hook_derived)"), "(int) 2\n");
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int HookDerived::value()", "return 42;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_virtual(&hook_derived)"), "(int) 42\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "int hook_file(int x)", "return x*3;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_file(4)"), "(int) 12\n");
}

TEST_F(CppInterpreterTest, HooksCoverRecursionAndInternalFunctions) {
  ASSERT_TRUE(cpp_->execute("int recursion_visits = 0; "
      "int recursive_hook(int n) { return n ? recursive_hook(n-1)+1 : 0; } "
      "static int internal_hook(int x) { return x+1; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "recursive_hook(int n)",
                           "++recursion_visits;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("recursive_hook(3)"), "(int) 3\n");
  EXPECT_EQ(cpp_->evaluate("recursion_visits"), "(int) 4\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "internal_hook(int x)", "return x+10;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("internal_hook(2)"), "(int) 12\n");
}

TEST_F(CppInterpreterTest, HooksSupportAggregateResultsAndRvalueReferences) {
  ASSERT_TRUE(cpp_->execute("struct HookResult { long a, b, c; }; "
      "HookResult hook_result(long x) { return {x,x+1,x+2}; } "
      "int hook_rvalue(int&& x) { return x+1; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "HookResult hook_result(long x)",
                           "result.c += x;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_result(2).c"), "(long) 6\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "hook_result(long x)", "return {x,10,20};", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_result(2).c"), "(long) 22\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Pre, "hook_rvalue(int&& x)", "x += 2;", error)) << error;
  EXPECT_EQ(cpp_->evaluate("hook_rvalue(3)"), "(int) 6\n");
}

TEST_F(CppInterpreterTest, UnsupportedTargetsFailWithoutBreakingTheSession) {
  ASSERT_TRUE(cpp_->execute("constexpr int constant_hook(int x) { return x; } "
      "int variadic_hook(int x, ...) { return x; } int declaration_hook(int x);"));
  std::string error;
  for (const char* signature : {"int constant_hook(int x)", "int variadic_hook(int x, ...)",
                                "int declaration_hook(int x)"}) {
    EXPECT_FALSE(cpp_->set_hook(HookKind::Mock, signature, "return 3;", error)) << signature;
    EXPECT_FALSE(error.empty());
  }
  EXPECT_EQ(cpp_->evaluate("constant_hook(2)"), "(int) 2\n");
}

TEST(FunctionHooks, OptimizedSessionKeepsCallsInterceptable) {
  std::string error;
  auto cpp = CppInterpreter::create({"-O2"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int optimized_hook(int x) { return x*2; } "
                         "int optimized_caller() { return optimized_hook(3); }"));
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "optimized_hook(int x)", "return x+10;", error)) << error;
  EXPECT_EQ(cpp->evaluate("optimized_caller()"), "(int) 13\n");
}

TEST_F(CppInterpreterTest, PostHooksRunOnlyOnNormalReturns) {
  ASSERT_TRUE(cpp_->execute("int exception_post = 0; int exception_seen = 0; "
      "int exception_hook(int x) { if (x) throw x; return 2; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Post, "exception_hook(int x)",
                           "++exception_post;", error)) << error;
  ASSERT_TRUE(cpp_->execute("try { exception_hook(3); } catch (int x) { exception_seen=x; }"));
  EXPECT_EQ(cpp_->evaluate("exception_post"), "(int) 0\n");
  EXPECT_EQ(cpp_->evaluate("exception_seen"), "(int) 3\n");
  EXPECT_EQ(cpp_->evaluate("exception_hook(0)"), "(int) 2\n");
  EXPECT_EQ(cpp_->evaluate("exception_post"), "(int) 1\n");
}

#ifdef __APPLE__
TEST_F(CppInterpreterTest, LibCommandMakesLibrarySymbolsAvailable) {
  LibraryManager libs;
  std::string hook_path;
  libs.set_load_hook([&](const std::string& path) {
    hook_path = path;
    return cpp_->load_library(path);
  });
  std::ostringstream out;
  dispatch(parse_command("%lib z"), libs, out);
  EXPECT_EQ(out.str().find("error: "), std::string::npos) << out.str();
  EXPECT_EQ(hook_path, libs.loaded().at(0).path);

  EXPECT_TRUE(cpp_->execute("extern \"C\" const char* zlibVersion();"));
  const std::optional<std::string> version = cpp_->evaluate("zlibVersion()");
  ASSERT_TRUE(version.has_value());
  EXPECT_EQ(version->rfind("(const char *) \"1.", 0), 0u) << *version;
}

TEST_F(CppInterpreterTest, LoadHookErrorRejectsLibrary) {
  LibraryManager libs;
  libs.set_load_hook([](const std::string&) { return std::string("rejected"); });
  std::ostringstream out;
  dispatch(parse_command("%lib z"), libs, out);
  EXPECT_EQ(out.str(), "error: rejected\n");
  EXPECT_TRUE(libs.loaded().empty());
}
#endif

}  // namespace
}  // namespace example
