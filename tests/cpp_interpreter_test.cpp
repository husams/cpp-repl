#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "cpp_symbols.hpp"
#include "editor_ui.hpp"
#include "highlight_repl.hpp"
#include "library_manager.hpp"
#include "loaded_symbols.hpp"
#include "native_symbol_exports.hpp"
#include "runtime_inspection.hpp"
#include "session_commands.hpp"

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

#ifdef __APPLE__
class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const char* name, std::optional<std::string> value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) previous_ = previous;
    if (value) setenv(name, value->c_str(), 1);
    else unsetenv(name);
  }
  ~ScopedEnvironmentVariable() {
    if (previous_) setenv(name_.c_str(), previous_->c_str(), 1);
    else unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

TEST(SdkPathCacheTest, ReusesSuccessfulDiscoveryAndHonorsEnvironmentAndOverrides) {
  const auto root = std::filesystem::temp_directory_path() /
      ("cpp_repl_sdk_cache_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  const auto bin = root / "bin";
  const auto sdk = root / "sdk";
  const auto fail_lookup = root / "fail-xcrun";
  std::filesystem::create_directories(bin);
  std::filesystem::create_directories(sdk);
  const auto counter = root / "xcrun-count";
  const auto script = bin / "xcrun";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\n"
        << "printf x >> '" << counter.string() << "'\n"
        << "if [ -f '" << fail_lookup.string() << "' ]; then exit 7; fi\n"
        << "printf '%s\\n' '" << sdk.string() << "'\n";
  }
  std::filesystem::permissions(
      script, std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write |
                  std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);

  ScopedEnvironmentVariable path("PATH", bin.string());
  ScopedEnvironmentVariable developer_dir("DEVELOPER_DIR", (root / "toolchain-a").string());
  ScopedEnvironmentVariable sdkroot("SDKROOT", sdk.string());
  ScopedEnvironmentVariable toolchains("TOOLCHAINS", "sdk-cache-test-toolchain-a");

  std::string error;
  std::ofstream(fail_lookup) << "fail once\n";
  auto first = CppInterpreter::create({}, error);
  ASSERT_NE(first, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 1u);
  // A failed xcrun result was not cached: the same environment retries and
  // stores the subsequent successful path.
  std::filesystem::remove(fail_lookup);
  first->complete("int sdk_cache_first_");
  first->complete("int sdk_cache_second_");
  EXPECT_EQ(std::filesystem::file_size(counter), 2u);

  // A changed SDK-selection environment forms a separate cache entry.
  ScopedEnvironmentVariable changed_developer_dir(
      "DEVELOPER_DIR", (root / "toolchain-b").string());
  auto second = CppInterpreter::create({}, error);
  ASSERT_NE(second, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 3u);

  // xcrun toolchain selection also participates in the cache key.
  ScopedEnvironmentVariable changed_toolchains("TOOLCHAINS", "sdk-cache-test-toolchain-b");
  auto third = CppInterpreter::create({}, error);
  ASSERT_NE(third, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 4u);

  // Unset and empty environment values are distinct xcrun inputs.
  ScopedEnvironmentVariable empty_sdkroot("SDKROOT", std::string());
  auto empty_sdkroot_session = CppInterpreter::create({}, error);
  ASSERT_NE(empty_sdkroot_session, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 5u);
  ScopedEnvironmentVariable unset_sdkroot("SDKROOT", std::nullopt);
  auto unset_sdkroot_session = CppInterpreter::create({}, error);
  ASSERT_NE(unset_sdkroot_session, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 6u);

  // Explicit sysroot forms are authoritative and avoid xcrun altogether.
  auto explicit_sysroot = CppInterpreter::create({"-isysroot", sdk.string()}, error);
  ASSERT_NE(explicit_sysroot, nullptr) << error;
  auto explicit_sysroot_equals =
      CppInterpreter::create({"--sysroot=" + sdk.string()}, error);
  ASSERT_NE(explicit_sysroot_equals, nullptr) << error;
  auto explicit_joined_sysroot =
      CppInterpreter::create({"-isysroot" + sdk.string()}, error);
  ASSERT_NE(explicit_joined_sysroot, nullptr) << error;
  EXPECT_EQ(std::filesystem::file_size(counter), 6u);

  std::filesystem::remove_all(root, ec);
}
#endif

std::string info_field(const std::string& text, const std::string& label);

TEST_F(CppInterpreterTest, TypeCommandReadsDeclaredTypesWithoutExecuting) {
  ASSERT_TRUE(cpp_->execute("namespace type_cmd { int x = 0; const int cx = 1; "
                           "int& ref = x; int values[3]; int* ptr = &x; int calls = 0; "
                           "int f(int x) { ++calls; return x; } "
                           "double f(double x) { ++calls; return x; } "
                           "void empty(); int variadic(int, ...); "
                           "struct S { int member; int get(double) const & noexcept; }; }"));
  LibraryManager libs;
  for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
           {"x", "(int)\n"}, {"cx", "(const int)\n"}, {"ref", "(int &)\n"},
           {"values", "(int[3])\n"}, {"ptr", "(int *)\n"},
           {"f(int)", "(int) -> int\n"}, {"empty", "() -> void\n"},
           {"variadic", "(int, ...) -> int\n"}, {"S::member", "(int)\n"},
           {"S::get", "(double) const & noexcept -> int\n"}}) {
    std::ostringstream out;
    const auto cmd = parse_command("%type 'type_cmd::" + entry.first + "'");
    EXPECT_TRUE(execute_command(libs, *cpp_, cmd, out).ok) << out.str();
    EXPECT_EQ(out.str(), entry.second) << entry.first;
  }
  std::string error = "old error";
  const auto overloads = cpp_->symbol_type("type_cmd::f", error);
  ASSERT_TRUE(overloads) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_NE(overloads->find("(int) -> int\n"), std::string::npos);
  EXPECT_NE(overloads->find("(double) -> double\n"), std::string::npos);
  EXPECT_FALSE(cpp_->symbol_type("type_cmd::missing", error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(cpp_->symbol_type("type_cmd", error));
  EXPECT_FALSE(cpp_->symbol_type("type_cmd::f(++type_cmd::calls)", error));
  EXPECT_EQ(cpp_->evaluate("type_cmd::calls"), "(int) 0\n");
  EXPECT_EQ(cpp_->evaluate("type_cmd::x"), "(int) 0\n");
}

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

TEST(HiddenSymbolsTest, CapturesStringValues) {
  std::string error;
  auto cpp = CppInterpreter::create({"-fvisibility=hidden"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  for (int i = 0; i != 2; ++i) {
    const auto value = cpp->evaluate("std::string(\"Sample\")");
    ASSERT_TRUE(value.has_value());
    EXPECT_NE(value->find("basic_string"), std::string::npos) << *value;
  }
}

TEST_F(CppInterpreterTest, CapturesDefaultVisibilityStringValues) {
  ASSERT_TRUE(cpp_->execute("#include <string>\n"));
  const auto value = cpp_->evaluate("std::string(\"Sample\")");
  ASSERT_TRUE(value.has_value());
  EXPECT_NE(value->find("basic_string"), std::string::npos) << *value;
}

TEST(HiddenSymbolsTest, DefinesStringReturningMethodFromHeader) {
  std::string error;
  auto cpp = CppInterpreter::create({"-fvisibility=hidden"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  const auto header = std::filesystem::path(__FILE__).parent_path() / "fixtures/newtag_person.hpp";
  ASSERT_TRUE(cpp->execute("#include \"" + header.string() + "\"\n"));
  ASSERT_TRUE(cpp->define_callable("std::string Person::getName() const",
                                 "return \"Sample\";", error)) << error;
  ASSERT_TRUE(cpp->execute("Person p;"));
  EXPECT_TRUE(cpp->execute("p.getName();"));
  const auto value = cpp->evaluate("p.getName()");
  ASSERT_TRUE(value.has_value());
  EXPECT_NE(value->find("basic_string"), std::string::npos) << *value;
  EXPECT_EQ(cpp->evaluate("p.getName() == \"Sample\""), "(bool) true\n");
}

TEST(HiddenSymbolsTest, RejectsMissingSymbolsAndKeepsHiddenDefinitionsUsable) {
  std::string error;
  auto cpp = CppInterpreter::create({"-fvisibility=hidden"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  ASSERT_TRUE(cpp->execute("int effects = 0; int missing_function(); extern int missing_global;"));
  EXPECT_FALSE(cpp->execute("int rejected = (++effects, missing_function());"));
  EXPECT_FALSE(cpp->execute("int rejected = (++effects, missing_global);"));
  EXPECT_EQ(cpp->evaluate("effects"), "(int) 0\n");
  EXPECT_TRUE(cpp->evaluate("std::string(\"Recovered\")").has_value());
  ASSERT_TRUE(cpp->execute("int hidden_function() { return 42; }"));
  EXPECT_EQ(cpp->evaluate("hidden_function()"), "(int) 42\n");
  ASSERT_TRUE(cpp->set_hook(HookKind::Mock, "hidden_function()", "return 7;", error)) << error;
  EXPECT_EQ(cpp->evaluate("hidden_function()"), "(int) 7\n");
  ASSERT_TRUE(cpp->restore("hidden_function()", error)) << error;
  EXPECT_EQ(cpp->evaluate("hidden_function()"), "(int) 42\n");
}

TEST(HiddenSymbolsTest, PrefersHiddenJitStorageOverSameNamedProcessSymbol) {
  void* process_storage = dlsym(RTLD_DEFAULT, "optind");
  ASSERT_NE(process_storage, nullptr);
  std::string error;
  auto cpp = CppInterpreter::create({"-fvisibility=hidden"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("extern \"C\" { int optind = 42; }"));
  EXPECT_EQ(cpp->evaluate("optind"), "(int) 42\n");
  const auto info = cpp->symbol_info("optind", error);
  ASSERT_TRUE(info.has_value()) << error;
  std::ostringstream native_address;
  native_address << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(process_storage);
  ASSERT_FALSE(info_field(*info, "Address").empty()) << *info;
  EXPECT_NE(info_field(*info, "Address"), native_address.str()) << *info;
}

TEST_F(CppInterpreterTest,
       RejectsThreadLocalPublicationBeforeRunningTheInitializer) {
  ASSERT_TRUE(cpp_->execute(
      "namespace tls_publication { extern thread_local int pending; "
      "int effects = 0; }"));
  std::string error;
  EXPECT_FALSE(cpp_->define_variable("tls_publication::pending",
                                    "(++tls_publication::effects, 7)", error));
  EXPECT_NE(error.find("thread-local"), std::string::npos);
  EXPECT_EQ(cpp_->evaluate("tls_publication::effects"), "(int) 0\n");
}

TEST_F(CppInterpreterTest, DefinesCallablesBeforeLoadingOneUnchangedNativeLibrary) {
  EXPECT_EQ(dlopen(UNRESOLVED_CALLABLES_LIBRARY, RTLD_NOW | RTLD_GLOBAL), nullptr);
  ASSERT_TRUE(cpp_->add_source(UNRESOLVED_CALLABLES_HEADER).ok);
  std::string error;
  for (const auto& item : std::vector<std::pair<std::string, std::string>>{
           {"unresolved_callables::adjust(int x)", "return x + 7;"},
           {"unresolved_callables::adjust(double x)", "return x * 2.5;"},
           {"unresolved_callables::transform(unresolved_callables::Value v, double factor)",
            "return {v.a * factor, v.b * factor, v.c * factor, v.d * factor};"},
           {"unresolved_callables::decorate(const std::string& s)", "return \"[\" + s + \"]\";"},
           {"unresolved_callables::Worker::hidden(int x) const & noexcept",
            "return this->value_ + x;"},
           {"unresolved_callables::Worker::scale(int x)", "return x * 3;"}}) {
    ASSERT_TRUE(cpp_->define_callable(item.first, item.second, error)) << error;
  }
  void* handle = dlopen(UNRESOLVED_CALLABLES_LIBRARY, RTLD_NOW | RTLD_GLOBAL);
  ASSERT_NE(handle, nullptr) << dlerror();
  auto probe = reinterpret_cast<int (*)()>(dlsym(handle, "callable_native_probe"));
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(probe(), 1); // Executes precompiled native callers, not JIT callers.
  EXPECT_EQ(cpp_->load_library(UNRESOLVED_CALLABLES_LIBRARY), "");
  EXPECT_EQ(cpp_->evaluate("unresolved_callables::callable_native_probe()"), "(int) 1\n");
  EXPECT_EQ(cpp_->evaluate("unresolved_callables::adjust(5)"), "(int) 12\n");
  EXPECT_EQ(cpp_->evaluate(
                "unresolved_callables::transform(unresolved_callables::Value{1,2,3,4}, 2.5).d"),
            "(double) 10.0\n");
  ASSERT_TRUE(cpp_->set_hook(
      HookKind::Mock,
      "unresolved_callables::transform(unresolved_callables::Value v, double factor)",
      "return {v.a * factor, v.b * factor, v.c * factor, 99};", error))
      << error;
  EXPECT_EQ(probe(), 0);
  EXPECT_EQ(cpp_->evaluate(
                "unresolved_callables::transform(unresolved_callables::Value{1,2,3,4}, 2.5).d"),
            "(double) 99.0\n");
  ASSERT_TRUE(cpp_->restore(
      "unresolved_callables::transform(unresolved_callables::Value v, double factor)", error))
      << error;
  EXPECT_EQ(probe(), 1);
  EXPECT_FALSE(cpp_->define_callable("unresolved_callables::adjust(int x)", "return 0;", error));
  EXPECT_NE(error.find("already"), std::string::npos);
  EXPECT_FALSE(
      cpp_->define_callable("unresolved_callables::Worker::run(int x) const", "return 0;", error));
  EXPECT_NE(error.find("existing implementation"), std::string::npos);
  // Handles stay loaded while native exports refer to session code.
}

TEST_F(CppInterpreterTest, DefinesVirtualMethodsAndOverridesBeforeNativeLoading) {
  ASSERT_EQ(dlopen(UNRESOLVED_VIRTUALS_LIBRARY, RTLD_NOW | RTLD_GLOBAL), nullptr);
  ASSERT_TRUE(cpp_->add_source(UNRESOLVED_VIRTUALS_HEADER).ok);
  std::string error;
  ASSERT_TRUE(cpp_->define_callable("virtual_fixture::Service::compute(int x) const",
                                    "return seed_ + x;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("virtual_fixture::Implementation::compute(int x) const",
                                    "return this->bias_ + x * 2;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("virtual_fixture::NativeTable::compute(int x) const",
                                    "return bias_ + x * 3;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("virtual_fixture::VirtualBase::compute(int x) const",
                                    "return bias_ + x * 4;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("virtual_fixture::Covariant::self()", "return this;", error))
      << error;
  void* handle = dlopen(UNRESOLVED_VIRTUALS_LIBRARY, RTLD_NOW | RTLD_GLOBAL);
  ASSERT_NE(handle, nullptr) << dlerror();
  const auto non_pure = reinterpret_cast<int (*)(int)>(dlsym(handle, "virtual_non_pure_probe"));
  const auto override = reinterpret_cast<int (*)(int)>(dlsym(handle, "virtual_override_probe"));
  const auto rtti = reinterpret_cast<int (*)()>(dlsym(handle, "virtual_rtti_probe"));
  ASSERT_NE(non_pure, nullptr);
  ASSERT_NE(override, nullptr);
  ASSERT_NE(rtti, nullptr);
  EXPECT_EQ(non_pure(2), 42);
  EXPECT_EQ(override(3), 106);
  EXPECT_EQ(rtti(), 1);
  auto native_table = reinterpret_cast<int (*)(int)>(dlsym(handle, "virtual_native_table_probe"));
  auto virtual_base = reinterpret_cast<int (*)(int)>(dlsym(handle, "virtual_base_probe"));
  auto covariant = reinterpret_cast<int (*)()>(dlsym(handle, "virtual_covariant_probe"));
  ASSERT_NE(native_table, nullptr);
  ASSERT_NE(virtual_base, nullptr);
  ASSERT_NE(covariant, nullptr);
  EXPECT_EQ(native_table(2), 206);
  EXPECT_EQ(virtual_base(2), 308);
  EXPECT_EQ(covariant(), 1);
  ASSERT_EQ(cpp_->load_library(UNRESOLVED_VIRTUALS_LIBRARY), "");
  EXPECT_EQ(cpp_->evaluate("virtual_fixture::virtual_non_pure_probe(2)"), "(int) 42\n");
  EXPECT_EQ(cpp_->evaluate("virtual_fixture::virtual_override_probe(3)"), "(int) 106\n");
  EXPECT_EQ(cpp_->evaluate("virtual_fixture::virtual_rtti_probe()"), "(int) 1\n");
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "virtual_fixture::Service::compute(int x) const",
                             "return x + 1000;", error))
      << error;
  EXPECT_EQ(non_pure(2), 1002);
  ASSERT_TRUE(cpp_->restore("virtual_fixture::Service::compute(int x) const", error)) << error;
  EXPECT_EQ(non_pure(2), 42);
  EXPECT_FALSE(cpp_->define_callable("virtual_fixture::NativeTable::anchor()", "", error));
  EXPECT_NE(error.find("existing implementation"), std::string::npos);
}

TEST(NativeSymbolExports, ConflictingBatchPublishesNoneOfItsNewSymbols) {
  NativeSymbolExports exports;
  std::string error;
  const auto address = reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, "malloc"));
  ASSERT_NE(address, 0u);
  const std::string name = "cpp_repl_atomic_virtual_export_probe";
  ASSERT_EQ(dlsym(RTLD_DEFAULT, name.c_str()), nullptr);
  EXPECT_FALSE(exports.publish({{name, address}, {"malloc", 1}}, error));
  EXPECT_NE(error.find("different address"), std::string::npos);
  EXPECT_EQ(dlsym(RTLD_DEFAULT, name.c_str()), nullptr);
  ASSERT_TRUE(exports.publish({{name, address}}, error)) << error;
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, name.c_str())), address);
}

TEST_F(CppInterpreterTest, VirtualDefinitionErrorsAllowRetryAndRejectPureDeclarations) {
  ASSERT_TRUE(cpp_->execute("namespace virtual_recovery { int absent(int); "
                            "struct Service { virtual int compute(int) const; }; "
                            "struct Abstract { virtual int compute(int) const = 0; }; }"));
  std::string error;
  EXPECT_FALSE(cpp_->define_callable("virtual_recovery::Abstract::compute(int x) const",
                                     "return x;", error));
  EXPECT_NE(error.find("pure virtual"), std::string::npos);
  EXPECT_FALSE(cpp_->define_callable("virtual_recovery::Service::compute(int x) const",
                                     "return nonexistent;", error));
  EXPECT_FALSE(cpp_->define_callable("virtual_recovery::Service::compute(int x) const",
                                     "return absent(x);", error));
  ASSERT_TRUE(cpp_->define_callable("virtual_recovery::Service::compute(int x) const",
                                    "return x * 3;", error))
      << error;
  ASSERT_TRUE(cpp_->execute("virtual_recovery::Service virtual_recovered; "
                            "virtual_recovery::Service* virtual_pointer = &virtual_recovered;"));
  EXPECT_EQ(cpp_->evaluate("virtual_pointer->compute(14)"), "(int) 42\n");
}

TEST_F(CppInterpreterTest, CallableDefinitionsRecoverAndRejectUnsupportedTargets) {
  ASSERT_TRUE(cpp_->execute(
      "namespace callable_recovery { int pending(int); int dependency(int); int absent(int); "
      "int existing(int x) { return x; } static int local(int); "
      "int variadic(int, ...); struct A { virtual int virtual_method(int) = 0; }; }"));
  std::string error;
  EXPECT_FALSE(
      cpp_->define_callable("callable_recovery::pending(int x)", "return no_such_name;", error));
  ASSERT_TRUE(cpp_->define_callable("callable_recovery::pending(int x)", "return x * 2;", error))
      << error;
  EXPECT_EQ(cpp_->evaluate("callable_recovery::pending(21)"), "(int) 42\n");
  EXPECT_FALSE(
      cpp_->define_callable("callable_recovery::dependency(int x)", "return absent(x);", error));
  ASSERT_TRUE(cpp_->define_callable("callable_recovery::dependency(int x)", "return x;", error))
      << error;
  for (const char* signature :
       {"callable_recovery::existing(int x)", "callable_recovery::local(int x)",
        "callable_recovery::variadic(int x, ...)", "callable_recovery::A::virtual_method(int x)",
        "callable_recovery::unknown(int x)", "callable_recovery::A::A()",
        "callable_recovery::A::operator()(int x)"}) {
    EXPECT_FALSE(cpp_->define_callable(signature, "return 0;", error)) << signature;
    EXPECT_FALSE(error.empty()) << signature;
  }
}

TEST_F(CppInterpreterTest, CallableDefinitionsPreserveReferencesPointersAndCLinkage) {
  ASSERT_TRUE(cpp_->execute("#include <string>\n"));
  ASSERT_TRUE(cpp_->execute("namespace callable_types { int storage = 42; int& reference(); "
                            "int (*callback())(int); int identity(int x) { return x; } "
                            "int invoke(int (*)(int), int); struct Ref { int get() &&; }; "
                            "extern \"C\" int supplied_c_function(int); "
                            "extern \"C\" std::string supplied_string_value(std::string); }"));
  std::string error;
  ASSERT_TRUE(cpp_->define_callable("callable_types::reference()", "return storage;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("callable_types::callback()", "return identity;", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("callable_types::invoke(int (*f)(int), int result)",
                                    "return f(result);", error))
      << error;
  ASSERT_TRUE(cpp_->define_callable("callable_types::Ref::get() &&", "return 9;", error)) << error;
  ASSERT_TRUE(
      cpp_->define_callable("callable_types::supplied_c_function(int x)", "return x + 1;", error))
      << error;
  EXPECT_EQ(cpp_->evaluate("&callable_types::reference() == &callable_types::storage"),
            "(bool) true\n");
  EXPECT_EQ(cpp_->evaluate("callable_types::invoke(callable_types::callback(), 17)"), "(int) 17\n");
  EXPECT_EQ(cpp_->evaluate("callable_types::Ref{}.get()"), "(int) 9\n");
  auto native = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "supplied_c_function"));
  ASSERT_NE(native, nullptr);
  EXPECT_EQ(native(41), 42);
  ASSERT_TRUE(cpp_->define_callable("callable_types::supplied_string_value(std::string s)",
                                    "return s + \"!\";", error))
      << error;
  auto native_string =
      reinterpret_cast<std::string (*)(std::string)>(dlsym(RTLD_DEFAULT, "supplied_string_value"));
  ASSERT_NE(native_string, nullptr);
  EXPECT_EQ(native_string("native"), "native!");
}

TEST_F(CppInterpreterTest,
       DefinesStorageBeforeLoadingASingleUndefinedLibrary) {
  LibraryManager libs;
  libs.set_load_hook([](const std::string& path) {
    return cpp_->load_library(path);
  });
  const auto missing = libs.load(UNRESOLVED_STORAGE_LIBRARY);
  ASSERT_FALSE(missing.ok);
  ASSERT_TRUE(cpp_->add_source(UNRESOLVED_STORAGE_HEADER).ok);
  std::string error;
  const std::string member = "unresolved_storage_fixture::State::";
  ASSERT_TRUE(cpp_->define_variable(member + "value_", "17 * 3LL", error))
      << error;
  ASSERT_TRUE(cpp_->define_variable(member + "label_",
                                    "std::string(\"created in REPL\")", error))
      << error;
  const auto loaded = libs.load(UNRESOLVED_STORAGE_LIBRARY);
  ASSERT_TRUE(loaded.ok) << loaded.message;
  EXPECT_EQ(cpp_->evaluate(member + "read()"), "(long long) 51\n");
  EXPECT_EQ(cpp_->evaluate(member + "label()"),
            "(const char *) \"created in REPL\"\n");
  const auto info = cpp_->symbol_info(member + "value_", error);
  ASSERT_TRUE(info) << error;
  void* published = dlsym(RTLD_DEFAULT, info_field(*info, "Mangled").c_str());
  ASSERT_NE(published, nullptr);
  const auto address = std::to_string(reinterpret_cast<std::uintptr_t>(published));
  EXPECT_EQ(cpp_->evaluate(member + "value_address() == "
                          "reinterpret_cast<const void*>(" + address + "ULL)"),
            "(bool) true\n");
}

TEST_F(CppInterpreterTest,
       DefinesMissingStaticMembersUsingTheirActualCppTypes) {
  ASSERT_TRUE(cpp_->execute("#include <string>\n#include <vector>\n"));
  for (unsigned index = 0; index < 2; ++index) {
    const auto space = "generic_storage_" + std::to_string(index);
    ASSERT_TRUE(cpp_->execute(
        "namespace " + space +
        " { int backing = 6; int make_value() { return 7; } "
        "class State { static long long hidden; public: static long long "
        "read() { return hidden; } "
        "static std::string text; static std::vector<int> values; static const "
        "int table[3]; "
        "static int (*callback)(int); static int& reference; }; }"));
    std::string error;
    const auto before = cpp_->symbol_list(
        "^<session>$", "^" + space + "::State::", error, false, true);
    ASSERT_TRUE(before) << error;
    EXPECT_NE(before->find(space + "::State::hidden\n"), std::string::npos);
    const auto member = space + "::State::";
    ASSERT_TRUE(
        cpp_->define_variable(member + "hidden", "make_value() * 6LL", error))
        << error;
    ASSERT_TRUE(cpp_->define_variable(
        member + "text", "std::string(\"answer \") + std::to_string(42)",
        error))
        << error;
    ASSERT_TRUE(cpp_->define_variable(member + "values",
                                      "std::vector<int>{2, 5, 8}", error))
        << error;
    ASSERT_TRUE(cpp_->define_variable(member + "table", "{3, 4, 5}", error))
        << error;
    ASSERT_TRUE(cpp_->define_variable(member + "callback",
                                      "+[](int x) { return x * 3; }", error))
        << error;
    ASSERT_TRUE(cpp_->define_variable(member + "reference", "backing", error))
        << error;
    EXPECT_EQ(cpp_->evaluate("int(" + member + "read())"), "(int) 42\n");
    EXPECT_EQ(cpp_->evaluate(member + "text.c_str()"),
              "(const char *) \"answer 42\"\n");
    EXPECT_EQ(cpp_->evaluate(member + "values[1] + " + member + "table[2]"),
              "(int) 10\n");
    EXPECT_EQ(cpp_->evaluate(member + "callback(4)"), "(int) 12\n");
    ASSERT_TRUE(cpp_->execute(member + "reference = 9;"));
    EXPECT_EQ(cpp_->evaluate(space + "::backing"), "(int) 9\n");
    EXPECT_EQ(cpp_->symbol_list("^<session>$", "^" + space + "::State::", error,
                                false, true),
              "");
  }
}

TEST_F(CppInterpreterTest,
       MissingVariableDefinitionErrorsPreserveStateAndPermitRetry) {
  ASSERT_TRUE(cpp_->execute(
      "namespace definition_recovery { int side_effects = 0; "
      "int existing = 4; extern int pending; void function(); }"));
  std::string error;
  EXPECT_FALSE(cpp_->define_variable("definition_recovery::existing",
                                     "++definition_recovery::side_effects",
                                     error));
  EXPECT_FALSE(
      cpp_->define_variable("definition_recovery::unknown", "1", error));
  EXPECT_FALSE(
      cpp_->define_variable("definition_recovery::function", "1", error));
  EXPECT_FALSE(
      cpp_->define_variable("definition_recovery::pending", " ", error));
  EXPECT_FALSE(cpp_->define_variable("definition_recovery::pending",
                                     "unknown_initializer_value", error));
  ASSERT_TRUE(
      cpp_->define_variable("definition_recovery::pending", "42", error))
      << error;
  EXPECT_EQ(cpp_->evaluate("definition_recovery::pending"), "(int) 42\n");
  EXPECT_EQ(
      cpp_->evaluate(
          "definition_recovery::existing + definition_recovery::side_effects"),
      "(int) 4\n");
}

TEST_F(CppInterpreterTest,
       LibraryStaticMembersResolveWithoutReplacingTheirStorage) {
  ASSERT_TRUE(cpp_->load_library(HOOK_TEST_LIBRARY).empty());
  ASSERT_TRUE(cpp_->execute("#include \"" HOOK_TEST_HEADER "\"\n"));
  std::string error;
  const auto unresolved = cpp_->symbol_list(
      "^<session>$", "^hook_library::StaticStorage::", error, false, true);
  ASSERT_TRUE(unresolved) << error;
  EXPECT_TRUE(unresolved->empty());
  EXPECT_EQ(cpp_->evaluate("int(hook_library::StaticStorage::total)"),
            "(int) 37\n");
  EXPECT_EQ(cpp_->evaluate("hook_library::StaticStorage::label"),
            "(const char *) \"native static storage\"\n");
  EXPECT_FALSE(
      cpp_->define_variable("hook_library::StaticStorage::total", "99", error));
  EXPECT_NE(error.find("existing storage"), std::string::npos) << error;
  EXPECT_EQ(cpp_->evaluate("int(hook_library::StaticStorage::total)"),
            "(int) 37\n");
}

TEST(CppCallables, DefinitionsAndRecoveryHonorSessionCompilerFlags) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->set_compile_options({"-DCALLABLE_VALUE=42"}, error)) << error;
  ASSERT_TRUE(
      cpp->execute("namespace scoped_callables { int answer(); void notify(); int effects = 0; }"));
  EXPECT_FALSE(cpp->define_callable("scoped_callables::answer()", "return unknown_value;", error));
  ASSERT_TRUE(cpp->define_callable("scoped_callables::answer()", "return CALLABLE_VALUE;", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("scoped_callables::notify()", "effects = CALLABLE_VALUE;", error))
      << error;
  EXPECT_EQ(cpp->evaluate("scoped_callables::effects"), "(int) 0\n");
  ASSERT_TRUE(cpp->execute("scoped_callables::notify();"));
  EXPECT_EQ(cpp->evaluate("scoped_callables::effects"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("scoped_callables::answer()"), "(int) 42\n");
}

TEST(CppVariables, FailedStaticDefinitionsRecoverWithSessionCompilerFlags) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->set_compile_options({"-DRECOVERED_VALUE=42"}, error))
      << error;
  ASSERT_TRUE(
      cpp->execute("namespace scoped_recovery { int unavailable(); "
                   "class State { static long long value; public: static long "
                   "long read() { return value; } }; }"));
  EXPECT_FALSE(cpp->define_variable("scoped_recovery::State::value",
                                    "unavailable()", error));
  EXPECT_FALSE(cpp->define_variable("scoped_recovery::State::value",
                                    "unknown_initializer", error));
  EXPECT_EQ(cpp->symbol_list("<session>", "^scoped_recovery::State::value$",
                             error, false, true),
            "scoped_recovery::State::value\n");
  ASSERT_TRUE(cpp->define_variable("scoped_recovery::State::value",
                                   "RECOVERED_VALUE", error))
      << error;
  EXPECT_EQ(cpp->evaluate("int(scoped_recovery::State::read())"), "(int) 42\n");
  const auto info = cpp->symbol_info("scoped_recovery::State::value", error);
  ASSERT_TRUE(info) << error;
  EXPECT_EQ(info_field(*info, "Definition"), "yes");
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

TEST(IncludeRecovery, StandardNamespaceSurvivesLinkFailure) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  EXPECT_FALSE(cpp->execute(
      "namespace std { int lookup_missing(); } int lookup_result = std::lookup_missing();"));
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  ASSERT_TRUE(cpp->execute("std::string recovered = \"ss\";"));
  EXPECT_EQ(cpp->evaluate("int(recovered.size())"), "(int) 2\n");
  ASSERT_TRUE(cpp->execute("int lookup_result = 7;"));
  EXPECT_EQ(cpp->evaluate("lookup_result"), "(int) 7\n");
}

TEST(IncludeRecovery, StandardNamespaceSurvivesParseFailure) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  EXPECT_FALSE(cpp->execute("namespace std { int lookup_broken = ; }"));
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  ASSERT_TRUE(cpp->execute("std::string recovered = \"ss\";"));
  EXPECT_EQ(cpp->evaluate("int(recovered.size())"), "(int) 2\n");
}

TEST(IncludeRecovery, FirstSystemHeaderNamespaceLookupSurvivesParseFailure) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  EXPECT_FALSE(cpp->execute("#include <string>\nnamespace std { int lookup_broken = ; }"));
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  // Parsing a rejected first include need not emit the header's inline code.
  // This regression covers name lookup; successful execution is tested above.
  ASSERT_TRUE(cpp->execute("static_assert(sizeof(std::string) > 0);"));
}

TEST(IncludeRecovery, EarlierNamespaceSurvivesRepeatedFailures) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("namespace Kept { int value = 42; }"));
  ASSERT_TRUE(cpp->execute("namespace Kept { int other = 7; }"));
  EXPECT_FALSE(cpp->execute("namespace Kept { int broken = ; }"));
  EXPECT_EQ(cpp->evaluate("Kept::value + Kept::other"), "(int) 49\n");
  EXPECT_FALSE(cpp->execute(
      "namespace Kept { int missing(); } int result = Kept::missing();"));
  EXPECT_EQ(cpp->evaluate("Kept::value + Kept::other"), "(int) 49\n");
}

TEST(IncludeRecovery, RestoresEveryOverloadAndPreservesSurvivingOverloads) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "int lookup_pick(int) { return 42; } int lookup_pick(double) { return 7; }"));
  EXPECT_FALSE(cpp->execute(
      "int lookup_pick(int); extern int absent; int result = absent;"));
  EXPECT_EQ(cpp->evaluate("lookup_pick(1)"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("lookup_pick(0.5)"), "(int) 7\n");
  EXPECT_FALSE(cpp->execute(
      "int lookup_pick(int); int lookup_pick(double); extern int absent; int result = absent;"));
  EXPECT_EQ(cpp->evaluate("lookup_pick(1)"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("lookup_pick(0.5)"), "(int) 7\n");
  EXPECT_FALSE(cpp->execute(
      "int lookup_pick(int); int lookup_pick(double); int broken = ;"));
  EXPECT_EQ(cpp->evaluate("lookup_pick(1)"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("lookup_pick(0.5)"), "(int) 7\n");
}

TEST(IncludeRecovery, EarlierClassDefinitionSurvivesRejectedForwardDeclaration) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("struct LookupRecord { int value = 42; };"));
  EXPECT_FALSE(cpp->execute("struct LookupRecord; int broken = ;"));
  EXPECT_EQ(cpp->evaluate("LookupRecord{}.value"), "(int) 42\n");
  EXPECT_FALSE(cpp->execute(
      "struct LookupRecord; extern int absent; int result = absent;"));
  EXPECT_EQ(cpp->evaluate("LookupRecord{}.value"), "(int) 42\n");
}

TEST(IncludeRecovery, DoesNotPublishNamespacesFromRejectedUserCode) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  EXPECT_FALSE(cpp->execute(
      "namespace Rejected { int leaked = 17; } extern int absent; int result = absent;"));
  EXPECT_FALSE(cpp->execute(
      "namespace Rejected { int other = 23; } extern int absent; int result = absent;"));
  EXPECT_FALSE(cpp->execute("int use = Rejected::leaked;"));
  ASSERT_TRUE(cpp->execute("namespace Rejected { int leaked = 42; }"));
  EXPECT_EQ(cpp->evaluate("Rejected::leaked"), "(int) 42\n");
}

TEST_F(CppInterpreterTest, UndoSurvivesAnonymousNamespace) {
  EXPECT_FALSE(cpp_->execute(
      "namespace { int undo_anon_local = 1; } extern int undo_anon_missing; "
      "int undo_anon_result = undo_anon_missing;"));
  ASSERT_TRUE(cpp_->execute("int undo_anon_result = 7; int undo_anon_local = 2;"));
  EXPECT_EQ(cpp_->evaluate("undo_anon_result + undo_anon_local"), "(int) 9\n");
  ASSERT_TRUE(cpp_->execute("namespace { int undo_anon_next = 3; }"));
  EXPECT_EQ(cpp_->evaluate("undo_anon_next"), "(int) 3\n");
}

TEST_F(CppInterpreterTest, UndoRemovesOnlyRejectedUsingDirectives) {
  ASSERT_TRUE(cpp_->execute(
      "namespace UndoKept { int undo_kept = 4; } using namespace UndoKept; "
      "namespace UndoLookup { int undo_leaked = 5; }"));
  EXPECT_FALSE(cpp_->execute(
      "using namespace UndoLookup; using namespace UndoKept; "
      "namespace UndoRejected { int value = 1; } using namespace UndoRejected; "
      "extern int undo_using_missing; int undo_using_result = undo_using_missing;"));
  EXPECT_FALSE(cpp_->execute("int undo_lookup_use = undo_leaked;"));
  EXPECT_EQ(cpp_->evaluate("undo_kept"), "(int) 4\n");
  ASSERT_TRUE(cpp_->execute(
      "namespace UndoRejected { int value = 9; } int undo_using_result = 7;"));
  EXPECT_EQ(cpp_->evaluate("UndoRejected::value"), "(int) 9\n");
  EXPECT_EQ(cpp_->evaluate("UndoLookup::undo_leaked"), "(int) 5\n");
}

TEST_F(CppInterpreterTest, UndoSurvivesNestedUsingDirectives) {
  EXPECT_FALSE(cpp_->execute(
      "namespace UndoNested { namespace Names { int source = 4; } "
      "namespace { int value = 1; } using namespace Names; } "
      "namespace { namespace { int undo_nested_value = 2; } } "
      "extern int undo_nested_missing; int undo_nested_result = undo_nested_missing;"));
  ASSERT_TRUE(cpp_->execute(
      "namespace UndoNested { int value = 7; } int undo_nested_value = 2;"));
  EXPECT_EQ(cpp_->evaluate("UndoNested::value + undo_nested_value"), "(int) 9\n");
}

TEST_F(CppInterpreterTest, FailedInstrumentedInputRollsBackAndPreservesHooks) {
  ASSERT_TRUE(cpp_->execute("int undo_hook_target(int x) { return x + 1; }"));
  std::string error;
  ASSERT_TRUE(cpp_->set_hook(HookKind::Mock, "undo_hook_target(int x)",
                             "return x + 10;", error)) << error;
  EXPECT_FALSE(cpp_->execute(
      "namespace { int undo_hook_local = 1; } "
      "int undo_hook_caller(int x) { return undo_hook_target(x); } "
      "extern int undo_hook_missing; int undo_hook_result = undo_hook_missing;"));
  ASSERT_TRUE(cpp_->execute(
      "int undo_hook_local = 2; int undo_hook_result = 7; "
      "int undo_hook_caller(int x) { return undo_hook_target(x) + 1; }"));
  EXPECT_EQ(cpp_->evaluate("undo_hook_caller(3)"), "(int) 14\n");
  EXPECT_EQ(cpp_->evaluate("undo_hook_local + undo_hook_result"), "(int) 9\n");
  ASSERT_TRUE(cpp_->restore("undo_hook_target(int)", error)) << error;
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

TEST(IncludeRecovery, FailedAddPreservesEarlierHeaderDeclarations) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  const auto source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "examples/include-recovery/failed.cpp";
  EXPECT_FALSE(cpp->add_source(source.string()).ok);
  EXPECT_TRUE(cpp->sources().empty());
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  ASSERT_TRUE(cpp->execute("std::string recovered = \"ss\";"));
  EXPECT_EQ(cpp->evaluate("int(recovered.size())"), "(int) 2\n");
}

TEST(IncludeRecovery, FailedAddPreservesNewSystemHeaderNamespaces) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  const auto source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "examples/include-recovery/failed.cpp";
  EXPECT_FALSE(cpp->add_source(source.string()).ok);
  EXPECT_TRUE(cpp->sources().empty());
  ASSERT_TRUE(cpp->execute("#include <string>\n"));
  ASSERT_TRUE(cpp->execute("std::string recovered = \"ss\";"));
  EXPECT_EQ(cpp->evaluate("int(recovered.size())"), "(int) 2\n");
}

TEST_F(CppInterpreterTest, AddSourceWithAnonymousNamespaceCanBeRetriedAfterRollback) {
  const auto source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "examples/undo-crash/anonymous.cpp";
  const auto previous_sources = cpp_->sources();
  EXPECT_FALSE(cpp_->add_source(source.string()).ok);
  EXPECT_EQ(cpp_->sources(), previous_sources);
  ASSERT_TRUE(cpp_->execute("int undo_local = 2; int undo_result = 7;"));
  EXPECT_EQ(cpp_->evaluate("undo_result + undo_local"), "(int) 9\n");

  // Use a new session to retry the identical source after supplying its import.
  std::string error;
  auto retry = CppInterpreter::create({}, error);
  ASSERT_NE(retry, nullptr) << error;
  EXPECT_FALSE(retry->add_source(source.string()).ok);
  ASSERT_TRUE(retry->execute("int undo_missing = 5;"));
  const auto result = retry->add_source(source.string());
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(retry->evaluate("undo_result + undo_local"), "(int) 6\n");
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

TEST(IncludeRecovery, AddedNamespaceAndCompiledCallerSurviveRejectedInputs) {
  SourceDir dir;
  const auto header = dir.write("billing.hpp", "#pragma once\nnamespace retained_project { struct "
                                               "Price { int quote() const { return 70; } }; "
                                               "extern int fee; int invoice(const Price&); }\n");
  const auto source =
      dir.write("billing.cpp", "#include \"billing.hpp\"\nnamespace retained_project { "
                               "int invoice(const Price& item) { return item.quote() + fee; } }\n");
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->add_source(header).ok);
  ASSERT_TRUE(cpp->define_variable("retained_project::fee", "5", error)) << error;
  ASSERT_TRUE(cpp->add_source(source).ok);
  ASSERT_TRUE(
      cpp->execute("retained_project::Price retained_price; "
                   "int retained_caller() { return retained_project::invoice(retained_price); }"));
  for (const auto* rejected : {"namespace retained_project { int broken = ; }",
                               "namespace retained_project { int missing(); } "
                               "int failed_result = retained_project::missing();"}) {
    EXPECT_FALSE(cpp->execute(rejected));
    EXPECT_EQ(cpp->evaluate("retained_project::fee"), "(int) 5\n");
    EXPECT_EQ(cpp->evaluate("retained_price.quote()"), "(int) 70\n");
    EXPECT_EQ(cpp->evaluate("retained_project::invoice(retained_price)"), "(int) 75\n");
    EXPECT_EQ(cpp->evaluate("retained_caller()"), "(int) 75\n");
  }
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

TEST_F(CppInterpreterTest, SymbolInfoSelectsTypedSignaturesWithoutExecuting) {
  ASSERT_TRUE(cpp_->execute("namespace typed_info { "
                            "using Index = long; struct Price {}; int calls = 0; "
                            "int subtotal(Price) { ++calls; return 1; } "
                            "int pick(int) { ++calls; return 2; } "
                            "double pick(double) { ++calls; return 3; } "
                            "int reference(const Price&) { ++calls; return 4; } "
                            "int index(Index) { ++calls; return 5; } "
                            "struct Probe { int get() & { return 6; } "
                            "int get() const & { return 7; } int get() && { return 8; } }; }"));
  std::string error;
  for (const auto* query :
       {"typed_info::subtotal(typed_info::Price)", "::typed_info::subtotal(typed_info::Price)",
        "typed_info::pick( int )", "typed_info::pick(double)",
        "typed_info::reference(const typed_info::Price &)",
        "typed_info::reference(typed_info::Price const&)", "typed_info::index(typed_info::Index)",
        "typed_info::index(long)", "typed_info::Probe::get() &", "typed_info::Probe::get() const &",
        "typed_info::Probe::get() &&"}) {
    const auto info = cpp_->symbol_info(query, error);
    ASSERT_TRUE(info.has_value()) << query << ": " << error;
    EXPECT_EQ(info->find("matches for"), std::string::npos) << *info;
    EXPECT_EQ(info_field(*info, "Definition"), "yes") << *info;
  }
  const auto integer = cpp_->symbol_info("typed_info::pick(int)", error);
  const auto floating = cpp_->symbol_info("typed_info::pick(double)", error);
  ASSERT_TRUE(integer && floating);
  EXPECT_NE(info_field(*integer, "Mangled"), info_field(*floating, "Mangled"));
  EXPECT_FALSE(cpp_->symbol_info("typed_info::pick(1)", error));
  EXPECT_FALSE(cpp_->symbol_info("typed_info::pick(short)", error));
  EXPECT_FALSE(cpp_->symbol_info("typed_info::Probe::get() const &&", error));
  EXPECT_EQ(cpp_->evaluate("typed_info::calls"), "(int) 0\n");
}

TEST_F(CppInterpreterTest, SymbolInfoMatchesAliasReferenceQualifiersWithoutExecuting) {
  ASSERT_TRUE(cpp_->execute(
      "namespace alias_info { using Count = int; using Ratio = double; using Pointer = int*; "
      "using Values = int[2]; "
      "int calls = 0; "
      "int take(Count, const Ratio&) { ++calls; return 1; } "
      "int take(Count, Ratio&) { ++calls; return 2; } "
      "int mixed(const Ratio&, const Count&) noexcept { ++calls; return 3; } "
      "int moving(const Ratio&&) { ++calls; return 4; } "
      "int qualified(const volatile Ratio&) { ++calls; return 5; } "
      "int pointer(Pointer const&) { ++calls; return 6; } "
      "int pointer(const int*&) { ++calls; return 7; } "
      "int array(const Values&) { ++calls; return 9; } "
      "struct Probe { int read(const Ratio&) const & noexcept { return 8; } }; }"));
  std::string error;
  const auto prefix =
      cpp_->symbol_info("alias_info::take(alias_info::Count, const alias_info::Ratio&)", error);
  ASSERT_TRUE(prefix) << error;
  const auto selected = info_field(*prefix, "Mangled");
  for (const auto* query : {"alias_info::take(alias_info::Count,alias_info::Ratio const&)",
                            "alias_info::take(int,alias_info::Ratio const &)",
                            "alias_info::take(alias_info::Count,double const&)",
                            "alias_info::take(int,const double&)"}) {
    const auto info = cpp_->symbol_info(query, error);
    ASSERT_TRUE(info) << query << ": " << error;
    EXPECT_EQ(info_field(*info, "Mangled"), selected) << *info;
    EXPECT_EQ(info->find("matches for"), std::string::npos) << *info;
  }
  for (const auto* query :
       {"alias_info::mixed(alias_info::Ratio const&,const alias_info::Count&) noexcept",
        "alias_info::mixed(const alias_info::Ratio&,alias_info::Count const&)",
        "alias_info::mixed(double const&,alias_info::Count const&) noexcept",
        "alias_info::moving(alias_info::Ratio const&&)",
        "alias_info::qualified(alias_info::Ratio const volatile&)",
        "alias_info::array(alias_info::Values const&)",
        "alias_info::Probe::read(alias_info::Ratio const&) const & noexcept"}) {
    ASSERT_TRUE(cpp_->symbol_info(query, error)) << query << ": " << error;
  }
  const auto mutable_ref =
      cpp_->symbol_info("alias_info::take(alias_info::Count,alias_info::Ratio&)", error);
  ASSERT_TRUE(mutable_ref) << error;
  EXPECT_NE(info_field(*mutable_ref, "Mangled"), selected);
  const auto pointer = cpp_->symbol_info("alias_info::pointer(alias_info::Pointer const&)", error);
  const auto pointer_canonical = cpp_->symbol_info("alias_info::pointer(int* const&)", error);
  const auto pointee_const = cpp_->symbol_info("alias_info::pointer(const int*&)", error);
  ASSERT_TRUE(pointer && pointer_canonical && pointee_const) << error;
  EXPECT_EQ(info_field(*pointer, "Mangled"), info_field(*pointer_canonical, "Mangled"));
  EXPECT_NE(info_field(*pointer, "Mangled"), info_field(*pointee_const, "Mangled"));
  for (const auto* query :
       {"alias_info::take(alias_info::Count,alias_info::Ratio volatile&)",
        "alias_info::moving(alias_info::Ratio const&)",
        "alias_info::qualified(alias_info::Ratio const&)", "alias_info::array(int (const&)[2])",
        "alias_info::take(alias_info::Count,alias_info::Ratio const& value)",
        "alias_info::take(++alias_info::calls,alias_info::Ratio const&)",
        "alias_info::take(alias_info::Count,alias_info::Ratio const&) noexcept"}) {
    EXPECT_FALSE(cpp_->symbol_info(query, error)) << query;
  }
  EXPECT_EQ(cpp_->evaluate("alias_info::calls"), "(int) 0\n");
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
  // Toolchains may ship the C runtime outside the host system directories.
  Dl_info runtime{};
  ASSERT_NE(dladdr(dlsym(RTLD_DEFAULT, "malloc"), &runtime), 0);
  ASSERT_NE(runtime.dli_fname, nullptr);
  const auto directory = std::filesystem::path(runtime.dli_fname).parent_path().string();
  const std::string system = "^" + std::regex_replace(directory, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)") + "/";
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

namespace example {
namespace {
TEST_F(CppInterpreterTest, RichCompletionKeepsOverloadsAndInsertionSeparate) {
  ASSERT_TRUE(cpp_->execute("int editor_over(int count) { return count; }"));
  ASSERT_TRUE(cpp_->execute("double editor_over(double value) { return value; }"));
  const auto result = cpp_->complete("editor_ov");
  EXPECT_TRUE(has(result.names, "editor_over"));
  std::vector<CompletionItem> overloads;
  for (const auto& item : result.items) {
    if (item.text == "editor_over") {
      overloads.push_back(item);
    }
  }
  ASSERT_EQ(overloads.size(), 2u);
  EXPECT_NE(overloads[0].label, overloads[1].label);
  for (const auto& item : overloads) {
    EXPECT_EQ(item.kind, SemanticKind::Function);
    EXPECT_NE(item.label.find('('), std::string::npos);
    EXPECT_FALSE(item.detail.empty());
    EXPECT_EQ(item.text, "editor_over");
  }
}

TEST_F(CppInterpreterTest, RichMemberCompletionRespectsAccessAndConstness) {
  ASSERT_TRUE(
      cpp_->execute("struct EditorBase { int inherited; }; "
                    "class EditorRecord : public EditorBase { int secret; public: int field; "
                    "int read() const { return field; } void write() {} }; "
                    "const EditorRecord editor_record{};"));
  const auto result = cpp_->complete("editor_record.");
  EXPECT_TRUE(has(result.names, "field"));
  EXPECT_TRUE(has(result.names, "inherited"));
  EXPECT_TRUE(has(result.names, "read"));
  EXPECT_FALSE(has(result.names, "secret"));
  EXPECT_FALSE(has(result.names, "write"));
  const auto field = std::find_if(result.items.begin(), result.items.end(),
                                  [](const auto& item) { return item.text == "field"; });
  ASSERT_NE(field, result.items.end());
  EXPECT_EQ(field->kind, SemanticKind::Field);
  EXPECT_EQ(field->detail, "int");
}

TEST_F(CppInterpreterTest, SemanticColorsResolveEditedLocalsWithoutExecuting) {
  ASSERT_TRUE(cpp_->execute("int editor_side_effect = 0;"));
  const std::string input = "struct EditorColor { int field; int method() { return field; } };\n"
                            "EditorColor local; local.method(); ++editor_side_effect;";
  const auto tokens = cpp_->semantic_tokens(input);
  auto colored = [&](const std::string& text, SemanticKind kind) {
    return std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
      return input.substr(token.begin, token.end - token.begin) == text && token.kind == kind;
    });
  };
  EXPECT_TRUE(colored("EditorColor", SemanticKind::Type));
  EXPECT_TRUE(colored("local", SemanticKind::Variable));
  EXPECT_TRUE(colored("field", SemanticKind::Field));
  EXPECT_TRUE(colored("method", SemanticKind::Method));
  EXPECT_TRUE(colored("editor_side_effect", SemanticKind::Variable));
  EXPECT_EQ(cpp_->evaluate("editor_side_effect"), "(int) 0\n");
  EXPECT_FALSE(has(cpp_->symbol_names(), "local"));
}
} // namespace
} // namespace example

namespace example {
namespace {
TEST_F(CppInterpreterTest, EditorCompletionWorksInsideEditedCodeAndAfterCalls) {
  ASSERT_TRUE(cpp_->execute("struct EditorChain { int member; EditorChain next() { return {}; } }; "
                            "EditorChain editor_chain;"));
  EXPECT_TRUE(has(cpp_->complete("editor_chain.next().me").names, "member"));
  EXPECT_TRUE(
      has(cpp_->complete("void editor_local_fn() {\n int editor_local_value = 0;\n editor_local_v")
              .names,
          "editor_local_value"));
  EXPECT_FALSE(has(cpp_->symbol_names(), "editor_local_value"));
}

TEST_F(CppInterpreterTest, EditorSignatureHelpTracksArgumentsAndOverloads) {
  ASSERT_TRUE(cpp_->execute("void editor_hint(int first, double second) {}"));
  const auto hint = cpp_->complete("editor_hint(1, ");
  ASSERT_FALSE(hint.signatures.empty());
  EXPECT_EQ(hint.active_parameter, 1u);
  EXPECT_NE(hint.signatures.front().find("double second"), std::string::npos);
}

TEST_F(CppInterpreterTest, EditorCachesFollowRedefinitionAndRejectedInput) {
  ASSERT_TRUE(
      cpp_->execute("struct EditorVersion { int old_field; }; EditorVersion editor_version;"));
  EXPECT_TRUE(has(cpp_->complete("editor_version.").names, "old_field"));
  ASSERT_TRUE(
      cpp_->execute("struct EditorVersion { double new_field; }; EditorVersion editor_version;"));
  const auto current = cpp_->complete("editor_version.");
  EXPECT_TRUE(has(current.names, "new_field"));
  EXPECT_FALSE(has(current.names, "old_field"));
  EXPECT_FALSE(cpp_->execute("int editor_bad = ;"));
  cpp_->complete("editor_bad");
  cpp_->semantic_tokens("editor_version.new_field");
  EXPECT_EQ(cpp_->evaluate("21 * 2"), "(int) 42\n");
}

TEST_F(CppInterpreterTest, EditorSupportsStandardLibraryAndNamespaceMembers) {
  ASSERT_TRUE(cpp_->execute("#include <vector>\n"));
  ASSERT_TRUE(cpp_->execute("std::vector<int> editor_vector; namespace editor_ns { int number; }"));
  EXPECT_TRUE(has(cpp_->complete("editor_vector.si").names, "size"));
  EXPECT_TRUE(has(cpp_->complete("editor_ns::num").names, "number"));
}

TEST_F(CppInterpreterTest, EditorCompletionRespectsPreprocessorContexts) {
  ASSERT_TRUE(cpp_->execute("struct EditorDirectiveType {}; int editor_directive_value;\n"
                            "struct EDITOR_DIRECTIVE_MACRO_TYPE {}; "
                            "int EDITOR_DIRECTIVE_MACRO_VALUE;\n"));
  ASSERT_TRUE(cpp_->execute("#define EDITOR_DIRECTIVE_MACRO 1\n"));

  const auto directive = cpp_->complete("#");
  EXPECT_TRUE(has(directive.names, "include"));
  EXPECT_FALSE(has(directive.names, "EditorDirectiveType"));
  EXPECT_FALSE(has(directive.names, "editor_directive_value"));

  const auto include = cpp_->complete("#i");
  EXPECT_TRUE(has(include.names, "include"));
  EXPECT_FALSE(has(include.names, "EditorDirectiveType"));

  const auto indented = cpp_->complete("int editor_before_directive;\n  #i");
  EXPECT_TRUE(has(indented.names, "include"));
  EXPECT_FALSE(has(indented.names, "EditorDirectiveType"));

  const auto macro_name = cpp_->complete("#ifdef EDITOR_DIRECTIVE_MAC");
  EXPECT_TRUE(has(macro_name.names, "EDITOR_DIRECTIVE_MACRO"));
  EXPECT_FALSE(has(macro_name.names, "EditorDirectiveType"));
  EXPECT_FALSE(has(macro_name.names, "editor_directive_value"));
  EXPECT_FALSE(has(macro_name.names, "EDITOR_DIRECTIVE_MACRO_TYPE"));
  EXPECT_FALSE(has(macro_name.names, "EDITOR_DIRECTIVE_MACRO_VALUE"));

  const auto macro_definition = cpp_->complete("#define EDITOR_NEW_MAC");
  EXPECT_FALSE(has(macro_definition.names, "EDITOR_DIRECTIVE_MACRO_TYPE"));
  EXPECT_FALSE(has(macro_definition.names, "EDITOR_DIRECTIVE_MACRO_VALUE"));

  const auto expression = cpp_->complete("#if EDITOR_DIRECTIVE_MAC");
  EXPECT_TRUE(has(expression.names, "EDITOR_DIRECTIVE_MACRO"));
  EXPECT_FALSE(has(expression.names, "EditorDirectiveType"));
  EXPECT_FALSE(has(expression.names, "editor_directive_value"));
  EXPECT_FALSE(has(expression.names, "EDITOR_DIRECTIVE_MACRO_TYPE"));
  EXPECT_FALSE(has(expression.names, "EDITOR_DIRECTIVE_MACRO_VALUE"));

  const auto builtin_macro = cpp_->complete("#ifdef __c");
  EXPECT_TRUE(has(builtin_macro.names, "__cplusplus"));
  EXPECT_FALSE(has(builtin_macro.names, "EDITOR_DIRECTIVE_MACRO_TYPE"));
}
} // namespace
} // namespace example

namespace example {
namespace {
TEST_F(CppInterpreterTest, SemanticOverlayPreservesLiteralsCommentsAndCommandColors) {
  ASSERT_TRUE(cpp_->execute("int editor_paint = 0;"));
  const std::string code = "// café\neditor_paint += 1; \"editor_paint\"; // editor_paint";
  replxx::Replxx::colors_t actual(utf8_length(code), replxx::Replxx::Color::DEFAULT);
  auto lexical = actual;
  highlight_input(code, lexical);
  highlight_editor_input(*cpp_, code, actual);
  const auto begin = utf8_length(std::string_view(code).substr(0, code.find("editor_paint")));
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (i >= begin && i < begin + std::string("editor_paint").size()) {
      EXPECT_EQ(actual[i], semantic_color(SemanticKind::Variable));
    } else {
      EXPECT_EQ(actual[i], lexical[i]) << i;
    }
  }
  const std::string shell = "%lib ``echo editor_paint``";
  actual.assign(utf8_length(shell), replxx::Replxx::Color::DEFAULT);
  lexical = actual;
  highlight_input(shell, lexical);
  highlight_editor_input(*cpp_, shell, actual);
  EXPECT_EQ(actual, lexical);
  const std::string embedded = "%define editor_paint cpp`editor_paint + 1`";
  actual.assign(utf8_length(embedded), replxx::Replxx::Color::DEFAULT);
  highlight_editor_input(*cpp_, embedded, actual);
  EXPECT_EQ(actual[embedded.rfind("editor_paint")], semantic_color(SemanticKind::Variable));
  EXPECT_EQ(cpp_->evaluate("editor_paint"), "(int) 0\n");
}

TEST_F(CppInterpreterTest, SemanticOverlayResolvesLexicalTypeNamesAsVariables) {
  ASSERT_TRUE(cpp_->execute("int string = 42; int vector = 7;"));
  for (const std::string input : {"string", "string + 1", "vector"}) {
    replxx::Replxx::colors_t colors(utf8_length(input), replxx::Replxx::Color::DEFAULT);
    highlight_editor_input(*cpp_, input, colors);
    const auto name_end = input.find(' ');
    const auto length = name_end == std::string::npos ? input.size() : name_end;
    for (std::size_t i = 0; i < length; ++i) {
      EXPECT_EQ(colors[i], semantic_color(SemanticKind::Variable)) << input;
    }
  }
  EXPECT_EQ(cpp_->evaluate("string + vector"), "(int) 49\n");
}

TEST_F(CppInterpreterTest, SemanticColorsRespectShadowingAndIncompleteInput) {
  const std::string code = "struct EditorShadow {}; void editor_shadow_fn(int param) { int "
                           "EditorShadow = param; EditorShadow += 1; }";
  const auto tokens = cpp_->semantic_tokens(code);
  const auto last = code.rfind("EditorShadow");
  EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
    return token.begin == last && token.kind == SemanticKind::Variable;
  }));
  EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
    return token.begin == code.find("param") && token.kind == SemanticKind::Parameter;
  }));
  cpp_->semantic_tokens("void editor_partial() { int value = ");
  EXPECT_EQ(cpp_->evaluate("6 * 7"), "(int) 42\n");
  EXPECT_TRUE(cpp_->semantic_tokens(std::string(70000, 'x')).empty());
}

TEST_F(CppInterpreterTest, EditorSeesSessionMacrosAndDoesNotCommitEditedDefines) {
  ASSERT_TRUE(cpp_->execute("#define EDITOR_MACRO(value) ((value) + 1)\n"));
  EXPECT_TRUE(has(cpp_->complete("EDITOR_MAC").names, "EDITOR_MACRO"));
  const std::string code = "int editor_macro_local = EDITOR_MACRO(2);";
  const auto tokens = cpp_->semantic_tokens(code);
  EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
    return code.substr(token.begin, token.end - token.begin) == "EDITOR_MACRO" &&
           token.kind == SemanticKind::Macro;
  }));
  cpp_->semantic_tokens("#define EDITOR_UNCOMMITTED 42\n");
  EXPECT_FALSE(has(cpp_->complete("EDITOR_UNCOMMIT").names, "EDITOR_UNCOMMITTED"));
  ASSERT_TRUE(cpp_->execute("#undef EDITOR_MACRO\n"));
  EXPECT_FALSE(has(cpp_->complete("EDITOR_MAC").names, "EDITOR_MACRO"));
  EXPECT_EQ(cpp_->evaluate("21 * 2"), "(int) 42\n");
}

TEST(EditorOptionsTest, PreservesUndefAndRedefinitionOfCommandLineMacros) {
  std::string error;
  auto cpp = CppInterpreter::create({"-DEDITOR_OPTION_MACRO=1"}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#undef EDITOR_OPTION_MACRO\n"));
  EXPECT_FALSE(has(cpp->complete("EDITOR_OPTION_MAC").names, "EDITOR_OPTION_MACRO"));
  ASSERT_TRUE(cpp->execute("#define EDITOR_OPTION_MACRO 9\n"));
  const auto colors = cpp->semantic_tokens("int editor_option = EDITOR_OPTION_MACRO;");
  EXPECT_FALSE(colors.empty());
  EXPECT_EQ(cpp->evaluate("EDITOR_OPTION_MACRO"), "(int) 9\n");
}
} // namespace
} // namespace example

namespace example {
TEST(EditorRuntimeTest, IostreamEditingDoesNotDamageTheSession) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  cpp->semantic_tokens("#include <iostream>");
  ASSERT_TRUE(cpp->execute("#include <iostream>\n"));
  cpp->semantic_tokens("std::cout << \"editor\" << std::endl;");
  EXPECT_EQ(cpp->evaluate("21 * 2"), "(int) 42\n");
}
} // namespace example

namespace example {
TEST(EditorRuntimeTest, RetainsVariadicAndPastingMacrosDuringAnalysis) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#define EDITOR_VARIADIC(...) __VA_ARGS__\n"));
  ASSERT_TRUE(cpp->execute("#define EDITOR_JOIN(a, b) a ## b\n"));
  const std::string code = "EDITOR_VARIADIC(int editor_variadic = 1;) int EDITOR_JOIN(editor_, "
                           "pasted) = 2; editor_variadic; editor_pasted;";
  const auto tokens = cpp->semantic_tokens(code);
  for (const auto& name : {std::string("editor_variadic"), std::string("editor_pasted")}) {
    EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
      return token.begin == code.rfind(name) && token.kind == SemanticKind::Variable;
    })) << name;
  }
}

TEST(EditorRuntimeTest, QualifiedRedefinitionsAndLocalShadowingUseCurrentDeclarations) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "namespace editor_current { int value = 1; int function(int x) { return x; } }"));
  ASSERT_TRUE(cpp->execute("namespace editor_current { double value = 2; }"));
  const auto result = cpp->complete("editor_current::val");
  ASSERT_FALSE(result.items.empty());
  EXPECT_EQ(result.items.front().detail, "double");
  ASSERT_TRUE(cpp->execute("int editor_shadow_global = 0;"));
  const auto local =
      cpp->complete("void local() { double editor_shadow_global = 2; editor_shadow_g");
  ASSERT_FALSE(local.items.empty());
  EXPECT_EQ(local.items.front().detail, "double");
}

TEST(EditorRuntimeTest, ReopenedAnonymousNamespacesKeepAllMembersVisible) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("namespace { int editor_anon_first = 1; }"));
  ASSERT_TRUE(cpp->execute("namespace { int editor_anon_second = 2; }"));
  const std::string code = "editor_anon_first + editor_anon_second;";
  const auto tokens = cpp->semantic_tokens(code);
  for (const auto& name : {std::string("editor_anon_first"), std::string("editor_anon_second")}) {
    EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
      return token.begin == code.find(name) && token.kind == SemanticKind::Variable;
    })) << name;
    EXPECT_TRUE(has(cpp->complete(name).names, name));
  }
  EXPECT_EQ(cpp->evaluate("editor_anon_first + editor_anon_second"), "(int) 3\n");
}

TEST(EditorRuntimeTest, NamespaceSuggestionsFollowUsingDirectives) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("namespace editor_visible { int editor_using_visible; } "
                           "namespace editor_hidden { int editor_using_hidden; } "
                           "using namespace editor_visible;"));
  const auto result = cpp->complete("editor_using_");
  EXPECT_TRUE(has(result.names, "editor_using_visible"));
  EXPECT_FALSE(has(result.names, "editor_using_hidden"));
}

TEST(EditorRuntimeTest, CompletionInsideIncompleteMemberInitializerReturns) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::string error;
    auto cpp = CppInterpreter::create({}, error);
    if (!cpp) {
      _exit(2);
    }
    for (const auto* partial : {"struct C { int x =", "struct C { int x = 1; int y =",
                                "void editor_partial() { int value =",
                                "namespace editor_partial_ns { struct C { int x ="}) {
      cpp->complete(partial);
    }
    _exit(cpp->evaluate("21 * 2") == std::optional<std::string>("(int) 42\n") ? 0 : 3);
  }
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  pid_t waited = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    waited = waitpid(child, &status, WNOHANG);
    if (waited != 0) {
      if (waited < 0 && errno == EINTR) {
        continue;
      }
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
} // namespace example
