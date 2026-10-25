#include "cpp_interpreter.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

#ifdef __linux__
#include <sys/uio.h>
#endif

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Interpreter/Value.h"
#include "cpp_inspect.hpp"
#include "cpp_lexer.hpp"
#include "cpp_symbols.hpp"
#include "definition_shadowing.hpp"
#include "interpreter_symbols.hpp"
#include "interpreter_undo.hpp"
#include "library_manager.hpp"
#include "llvm/Config/llvm-config.h"
#if LLVM_VERSION_MAJOR >= 22
#include "clang/Interpreter/IncrementalExecutor.h"
#endif
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "loaded_symbols.hpp"
#include "native_symbol_exports.hpp"
#include "project_compilation_database.hpp"
#include "runtime_inspection.hpp"
#include "source_compile_options.hpp"
#include "source_reload.hpp"

// Set by CMake from the LLVM package that was linked.
#ifndef CPP_REPL_LLVM_PREFIX
#define CPP_REPL_LLVM_PREFIX "/opt/homebrew/opt/llvm"
#endif
#ifndef CPP_REPL_RESOURCE_DIR
#define CPP_REPL_RESOURCE_DIR CPP_REPL_LLVM_PREFIX "/lib/clang/22"
#endif

namespace example {

namespace {

void print_value(const clang::Value& value, llvm::raw_ostream& out) {
#if LLVM_VERSION_MAJOR >= 22
  std::string printed;
  llvm::raw_string_ostream stream(printed);
  value.print(stream);
  out << unversioned_name(std::move(printed));
#else
  // Clang 21 captures values but its Value::print() is still a placeholder.
  if (!value.hasValue()) return;
  const auto type = value.getType().getCanonicalType();
  out << "(" << unversioned_name(type.getAsString(value.getASTContext().getPrintingPolicy())) << ") ";
  auto floating = [&](auto number) {
    std::ostringstream text;
    text.precision(std::numeric_limits<decltype(number)>::max_digits10);
    text << number;
    std::string data = text.str();
    if (data.find_first_of(".eEinfNaINF") == std::string::npos) data += ".0";
    out << data;
  };
  switch (value.getKind()) {
    case clang::Value::K_Bool: out << (value.getBool() ? "true" : "false"); break;
#define REPL_INTEGER(Name) case clang::Value::K_##Name: out << +value.get##Name(); break
    REPL_INTEGER(Char_S); REPL_INTEGER(Char_U); REPL_INTEGER(SChar); REPL_INTEGER(UChar);
    REPL_INTEGER(Short); REPL_INTEGER(UShort); REPL_INTEGER(Int); REPL_INTEGER(UInt);
    REPL_INTEGER(Long); REPL_INTEGER(ULong); REPL_INTEGER(LongLong); REPL_INTEGER(ULongLong);
#undef REPL_INTEGER
    case clang::Value::K_Float: floating(value.getFloat()); break;
    case clang::Value::K_Double: floating(value.getDouble()); break;
    case clang::Value::K_LongDouble: floating(value.getLongDouble()); break;
    case clang::Value::K_PtrOrObj: {
      const auto* pointer = value.getPtr();
      if (!pointer) { out << "nullptr"; break; }
#ifdef __linux__
      if (type->isPointerType() && type->getPointeeType()->isCharType()) {
        char buffer[4096];
        const iovec local{buffer, sizeof(buffer)};
        const iovec remote{const_cast<void*>(pointer), sizeof(buffer)};
        const auto count = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (count > 0) {
          const auto* end = static_cast<const char*>(std::memchr(buffer, 0, count));
          if (end) {
            out << '"'; out.write_escaped(llvm::StringRef(buffer, end - buffer)); out << '"';
            break;
          }
        }
      }
#endif
      out << pointer;
      break;
    }
    default: out << "<unavailable>"; break;
  }
  out << '\n';
#endif
}

bool report(llvm::Error error) {
  llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "error: ");
  return false;
}

#ifdef __APPLE__
std::string sdk_path() {
  std::string out;
  if (FILE* p = popen("xcrun --show-sdk-path 2>/dev/null", "r")) {
    char buf[1024];
    while (fgets(buf, sizeof buf, p) != nullptr) out += buf;
    pclose(p);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}
#endif

// clang-repl derives these from its own location in the LLVM install; an
// embedding executable has to name them: Clang's builtin headers, and on macOS
// LLVM's libc++ headers plus the SDK for the C library headers.
std::vector<std::string> platform_args() {
  std::vector<std::string> args = {"-resource-dir", CPP_REPL_RESOURCE_DIR};
#ifdef __APPLE__
  args.insert(args.end(), {"-nostdinc++", "-isystem", CPP_REPL_LLVM_PREFIX "/include/c++/v1"});
  const std::string sdk = sdk_path();
  if (!sdk.empty()) args.insert(args.end(), {"-isysroot", sdk});
#endif
  if (isatty(STDERR_FILENO)) args.emplace_back("-fcolor-diagnostics");
  return args;
}

std::vector<std::string> compiler_arguments(
    const std::vector<std::string>& args) {
  auto all = platform_args();
  all.insert(all.end(), args.begin(), args.end());
  // Preserve call boundaries for hooks, including optimized sessions.
  all.insert(all.end(), {"-fno-inline", "-fno-inline-functions"});
#ifdef __linux__
  // JIT data can be more than 2 GiB away on x86_64; use GOT references.
  all.emplace_back("-fPIC");
#endif
#if defined(__linux__) && defined(__x86_64__) && \
    defined(CPP_REPL_NATIVE_CALL_PATCHES)
  all.emplace_back("-fcf-protection=branch");
#endif
  return all;
}

// A compiler for the interpreter: platform_args() plus the user's flags.
llvm::Expected<std::unique_ptr<clang::CompilerInstance>>
make_compiler(const std::vector<std::string>& args, bool shadow_definitions = true) {
  // Clang's real VFS applies -working-directory to the process as well. It
  // describes the build command, not where the REPL should continue running.
  struct DirectoryScope {
    std::filesystem::path original;
    DirectoryScope() {
      std::error_code error;
      original = std::filesystem::current_path(error);
    }
    ~DirectoryScope() {
      if (original.empty()) return;
      std::error_code error;
      std::filesystem::current_path(original, error);
      if (error)
        llvm::errs() << "error: cannot restore REPL directory: "
                     << error.message() << '\n';
    }
  } directory_scope;
  // The builder keeps the `const char*` pointers, so `all` must outlive
  // CreateCpp().
  const auto all = compiler_arguments(args);
  std::vector<const char*> argv;
  for (const std::string& a : all) argv.push_back(a.c_str());

  clang::IncrementalCompilerBuilder builder;
  builder.SetCompilerArgs(argv);
  auto compiler = builder.CreateCpp();
  if (compiler && shadow_definitions) {
    enable_definition_shadowing(**compiler);
  }
  return compiler;
}

std::string compile_file_path(const std::string& path, std::string& error) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(expand_home(path), ec);
  const auto file = ec ? std::filesystem::path()
                       : std::filesystem::weakly_canonical(absolute, ec);
  if (ec) {
    error = "cannot resolve filename: " + path + ": " + ec.message();
    return "";
  }
  if (std::filesystem::is_directory(file, ec)) {
    error = "not a source file: " + path;
    return "";
  }
  if (file.string().find_first_of("\"\n\r") != std::string::npos) {
    error = "unsupported quote or newline in filename: " + path;
    return "";
  }
  return file.string();
}

std::string format_compile_flags(const std::vector<std::string>& flags) {
  if (flags.empty()) return "(none)";
  std::ostringstream result;
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (i) result << ' ';
    if (flags[i].empty() ||
        flags[i].find_first_of(" \t\n\r\"'\\") != std::string::npos)
      result << std::quoted(flags[i]);
    else
      result << flags[i];
  }
  return result.str();
}

// True if the input ends in an expression without ';', whose value the
// interpreter keeps for printing.
bool value_pending(const clang::PartialTranslationUnit& ptu) {
  for (const clang::Decl* d : ptu.TUPart->decls()) {
    const auto* stmt = llvm::dyn_cast<clang::TopLevelStmtDecl>(d);
    if (stmt != nullptr && stmt->isSemiMissing()) return true;
  }
  return false;
}

// The external symbols `module` refers to that the JIT cannot resolve (from
// earlier inputs, loaded libraries or the process), demangled.
std::vector<std::string> missing_symbols(llvm::orc::LLJIT& jit,
                                         const llvm::Module& module) {
  std::vector<std::string> missing;
  auto check = [&](const llvm::GlobalValue& gv) {
    if (!gv.isDeclaration() || gv.hasExternalWeakLinkage() || !gv.hasName()) return;
    if (const auto* f = llvm::dyn_cast<llvm::Function>(&gv); f != nullptr && f->isIntrinsic()) {
      return;
    }
    auto addr = lookup_symbol(jit, gv.getName());
    if (addr) return;
    llvm::consumeError(addr.takeError());
    missing.push_back(llvm::demangle(gv.getName()));
  };
  for (const llvm::Function& f : module) check(f);
  for (const llvm::GlobalVariable& g : module.globals()) check(g);
  return missing;
}

}  // namespace

CppInterpreter::CppInterpreter(std::unique_ptr<clang::Interpreter> interp,
                               llvm::orc::LLJIT& jit,
                               std::vector<std::string> args,
                               std::unique_ptr<ProjectCompilationDatabase> database)
    : native_exports_(std::make_unique<NativeSymbolExports>()),
      interp_(std::move(interp)), jit_(jit), hooks_(*interp_, jit_), args_(std::move(args)),
      database_(std::move(database)) {
  source_reload_ = std::make_unique<SourceReload>(*interp_->getCompilerInstance());
}

CppInterpreter::~CppInterpreter() = default;

std::unique_ptr<CppInterpreter> CppInterpreter::create(
    const std::vector<std::string>& args, std::string& error) {
  auto database = ProjectCompilationDatabase::load(error);
  if (!database) return nullptr;
  std::error_code cwd_error;
  const auto directory = std::filesystem::current_path(cwd_error);
  if (cwd_error) {
    error = cwd_error.message();
    return nullptr;
  }
  const auto flags = normalize_compile_flags(args, directory.string(), error);
  if (!flags) return nullptr;
  static std::once_flag init;
  std::call_once(init, [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    // Keep JIT-ed printf/std::cout output, value printing (llvm::outs()) and
    // the REPL's own output in program order, even when piped.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    llvm::outs().SetUnbuffered();
  });

  auto ci = make_compiler(*flags);
  if (!ci) {
    error = llvm::toString(ci.takeError());
    return nullptr;
  }
  // Retain access to the JIT for hidden-symbol lookups. LLVM 22's executor
  // interface no longer exposes it. Keep Clang's native target/debugger setup.
  const llvm::Triple triple((*ci)->getTargetOpts().Triple);
  llvm::Expected<llvm::orc::JITTargetMachineBuilder> target =
      triple == llvm::Triple(llvm::sys::getProcessTriple())
          ? llvm::orc::JITTargetMachineBuilder::detectHost()
          : llvm::orc::JITTargetMachineBuilder(triple);
  if (!target) {
    error = llvm::toString(target.takeError());
    return nullptr;
  }
  auto builder = std::make_unique<llvm::orc::LLJITBuilder>();
  builder->setJITTargetMachineBuilder(std::move(*target));
  builder->setPrePlatformSetup([](llvm::orc::LLJIT& jit) {
    llvm::consumeError(llvm::orc::enableDebuggerSupport(jit));
    return llvm::Error::success();
  });
  // The builder retains the callback: do not capture a stack reference.
  auto jit = std::make_shared<llvm::orc::LLJIT*>(nullptr);
  builder->setNotifyCreatedCallback([jit](llvm::orc::LLJIT& created) {
    *jit = &created;
    return llvm::Error::success();
  });
#if LLVM_VERSION_MAJOR >= 22
  auto executor = std::make_unique<clang::IncrementalExecutorBuilder>();
  executor->JITBuilder = std::move(builder);
  auto interp = clang::Interpreter::create(std::move(*ci), std::move(executor));
#else
  auto interp = clang::Interpreter::create(std::move(*ci), std::move(builder));
#endif
  if (!interp) {
    error = llvm::toString(interp.takeError());
    return nullptr;
  }
  // Ensure the callback has run before borrowing the executor's JIT.
  auto engine = (*interp)->getExecutionEngine();
  if (!engine) {
    error = llvm::toString(engine.takeError());
    return nullptr;
  }
#if LLVM_VERSION_MAJOR == 21
  // Clang 21 parses its runtime PTU without executing it. Emit the missing
  // value-capture tag with Clang's own type and linker name in this JIT.
  const std::string runtime_tag =
      "__clang_Interpreter_NewTag __cpp_repl_runtime_newtag asm(\"" +
      (*jit)->mangle("__ci_newtag") + "\");";
  if (auto runtime = (*interp)->ParseAndExecute(runtime_tag)) {
    error = llvm::toString(std::move(runtime));
    return nullptr;
  }
#endif
  return std::unique_ptr<CppInterpreter>(
      new CppInterpreter(std::move(*interp), **jit, *flags, std::move(database)));
}

// ParseAndExecute(), split up so a failed Execute can be undone.
bool CppInterpreter::run(const std::string& code, clang::Value& value) {
  if (source_input_.empty()) reload_changed_sources();
  if (!global_options_changed_ || compile_scope_active_)
    return run_raw(code, value);
  SourceCompileOptions options;
  options.arguments = args_;
  std::error_code ec;
  options.directory = std::filesystem::current_path(ec).string();
  if (ec) {
    llvm::errs() << "error: " << ec.message() << '\n';
    return false;
  }
  std::string error;
  if (!reference_directory_.empty()) options.directory = reference_directory_;
  const bool result = with_compile_options(
      options, [&] { return run_raw(code, value); }, error);
  if (!error.empty()) llvm::errs() << "error: " << error << '\n';
  return result;
}

bool CppInterpreter::run_raw(const std::string& code, clang::Value& value,
                             std::vector<std::string>* abi_exports) {
  if (has_top_level_return(code)) {  // would crash the parser
    llvm::errs() << "error: 'return' is only allowed inside a function\n";
    return false;
  }
  DefinitionTransaction definitions(*interp_->getCompilerInstance());
  bool source_committed = false;
#if LLVM_VERSION_MAJOR >= 22
  auto source_scope = llvm::scope_exit([&] { source_reload_->finish(source_committed); });
#else
  auto source_scope = llvm::make_scope_exit([&] { source_reload_->finish(source_committed); });
#endif
  std::string source_error;
  if (!source_reload_->begin(source_input_, reloading_source_, source_error)) {
    llvm::errs() << "error: " << source_error << '\n';
    return false;
  }
  llvm::Expected<clang::PartialTranslationUnit&> ptu = parse_input(*interp_, code);
  if (!ptu) return report(ptu.takeError());
  if (ptu->TheModule) {
    const auto instrumented = hooks_.instrument(*ptu);
    std::vector<std::string> emitted_abi;
    if (abi_exports) {
      // Itanium ABI support emitted by Clang for a virtual definition: tables,
      // RTTI and adjusting/covariant thunks must be visible to native imports
      // alongside the method itself. Preserve hidden/private RTTI visibility;
      // export only definitions which Clang made visible to native consumers.
      for (const auto& global : ptu->TheModule->global_values()) {
        const auto name = global.getName();
        if (!global.isDeclarationForLinker() && !global.hasLocalLinkage() &&
            global.getVisibility() == llvm::GlobalValue::DefaultVisibility &&
            (name.starts_with("_ZTV") || name.starts_with("_ZTT") || name.starts_with("_ZTI") ||
             name.starts_with("_ZTS") || name.starts_with("_ZTC") || name.starts_with("_ZTh") ||
             name.starts_with("_ZTv") || name.starts_with("_ZTc"))) {
          emitted_abi.push_back(name.str());
        }
      }
    }
    // An unresolved symbol would fail in the JIT, and ORC keeps retrying the
    // failed module's initializer on every later input (its platform does not
    // forget initializers of removed modules), so check before executing.
    const std::vector<std::string> missing =
        missing_symbols(jit_, *ptu->TheModule);
    if (!missing.empty()) {
      hooks_.discard(instrumented);
      std::string list;
      for (const std::string& name : missing)
        list += (list.empty() ? "" : ", ") + name;
      llvm::errs() << "error: undefined symbol"
                   << (missing.size() > 1 ? "s" : "") << ": " << list << '\n';
      if (llvm::Error u = undo_input(*interp_, ptu->TUPart)) report(std::move(u));
      return false;
    }
    if (llvm::Error e = interp_->Execute(*ptu)) {
      hooks_.discard(instrumented);
      report(std::move(e));
      // Drop the input whose code failed to link (e.g. a missing symbol):
      // left in place, every later input retries its initializer and fails.
      if (llvm::Error u = undo_input(*interp_, ptu->TUPart)) report(std::move(u));
      return false;
    }
    if (abi_exports) {
      *abi_exports = std::move(emitted_abi);
    }
  }
  definitions.commit();
  source_committed = true;
  if (!value_pending(*ptu)) return true;
  // The value waits in the interpreter; ParseAndExecute() hands it to a
  // Value*. An empty input does that without running anything, and Undo()
  // removes the empty input again.
  if (llvm::Error e = interp_->ParseAndExecute("", &value)) return report(std::move(e));
  if (llvm::Error e = undo_input(*interp_, nullptr)) report(std::move(e));
  return true;
}

bool CppInterpreter::execute(const std::string& code) {
  clang::Value value;
  if (!run(code, value)) return false;
  if (value.isValid() && !value.isVoid()) print_value(value, llvm::outs());
  // An object value runs its destructor here, while its code is still loaded.
  value.clear();
  return true;
}

bool CppInterpreter::set_hook(HookKind kind, const std::string& signature,
                               const std::string& body, std::string& error, const std::string& caller) {
  error.clear();
  if (global_options_changed_ && !compile_scope_active_) {
    return with_compile_options(
        source_compile_options(""),
        [&] { return hooks_.set(kind, signature, body, error, caller); },
        error);
  }
  return hooks_.set(kind, signature, body, error, caller);
}

bool CppInterpreter::restore(const std::string& signature, std::string& error, const std::string& caller) {
  error.clear();
  if (global_options_changed_ && !compile_scope_active_) {
    return with_compile_options(
        source_compile_options(""),
        [&] { return hooks_.restore(signature, error, caller); }, error);
  }
  return hooks_.restore(signature, error, caller);
}

std::vector<ActiveHook> CppInterpreter::active_hooks() const {
  return hooks_.active();
}

bool CppInterpreter::define_callable(const std::string& signature, const std::string& body,
                                     std::string& error) {
  error.clear();
  if (global_options_changed_ && !compile_scope_active_) {
    return with_compile_options(
        source_compile_options(""), [&] { return define_callable(signature, body, error); }, error);
  }
  const auto callable = hooks_.definition(signature, error);
  if (!callable) {
    return false;
  }
  auto address = lookup_symbol(jit_, callable->mangled);
  if (address && address->getValue()) {
    error = "function already resolves to an existing implementation: " + signature;
    return false;
  }
  if (!address) {
    llvm::consumeError(address.takeError());
  }
  if (!native_exports_->prepare(error)) {
    return false;
  }
  clang::Value value;
  std::vector<std::string> exports;
  std::string prefix;
  for (unsigned i = 0; i < callable->template_specializations; ++i) prefix += "template<> ";
  if (!run_raw(prefix + "__attribute__((used)) " + callable->declaration + " {\n" + body + "\n}\n", value,
               &exports)) {
    discard_failed_callable_definition(*callable->function);
    error = "could not define callable: " + signature;
    return false;
  }
  exports.insert(exports.begin(), callable->mangled);
  std::vector<std::pair<std::string, std::uintptr_t>> addresses;
  for (const auto& symbol : exports) {
    auto implementation = lookup_symbol(jit_, symbol);
    if (!implementation || !implementation->getValue()) {
      if (!implementation) {
        llvm::consumeError(implementation.takeError());
      }
      error = "callable is defined, but an emitted symbol address is unavailable: " + symbol;
      return false;
    }
    addresses.emplace_back(symbol, implementation->getValue());
  }
  if (!native_exports_->publish(addresses, error)) {
    error = "callable is defined in the session, but native publication failed: " + error;
    return false;
  }
  return true;
}

bool CppInterpreter::define_variable(const std::string& name,
                                     const std::string& initializer,
                                     std::string& error) {
  error.clear();
  if (initializer.find_first_not_of(" \t\n\r") == std::string::npos) {
    error = "missing C++ initializer";
    return false;
  }
  const auto variable =
      variable_definition(interp_->getASTContext(), name, error);
  if (!variable) return false;
  if (variable->defined) {
    error = "variable already has a C++ definition: " + name;
    return false;
  }
  if (variable->thread_local_storage) {
    error = "native publication of thread-local storage is not supported: " + name;
    return false;
  }
  auto address = lookup_symbol(jit_, variable->mangled);
  if (address && address->getValue()) {
    error = "variable already resolves to existing storage: " + name;
    return false;
  }
  if (!address) llvm::consumeError(address.takeError());
  if (!native_exports_->prepare(error)) return false;
  // Native imports may use even constant members never ODR-used in the REPL.
  // Force their storage to be emitted before looking up its JIT address.
  if (!execute("__attribute__((used)) " + variable->declaration + " = " +
               initializer + ";\n")) {
    discard_failed_variable_definition(interp_->getASTContext(), name);
    error = "could not define variable: " + name;
    return false;
  }
  auto storage = lookup_symbol(jit_, variable->mangled);
  if (!storage || !storage->getValue()) {
    if (!storage) llvm::consumeError(storage.takeError());
    error = "variable is defined, but its storage address is unavailable: " + name;
    return false;
  }
  if (!native_exports_->publish(variable->mangled, storage->getValue(), error)) {
    error = "variable is defined in the session, but native publication failed: " + error;
    return false;
  }
  return true;
}

std::optional<std::string> CppInterpreter::symbol_list(
    const std::string& library_regex, const std::string& name_regex,
    std::string& error, bool include_system, bool undefined_only) {
  error.clear();
  std::regex library, name;
  auto compile = [&](const std::string& text, std::regex& regex,
                     const char* flag) {
    try {
      regex.assign(text, std::regex::ECMAScript);
      return true;
    } catch (const std::regex_error& failure) {
      error = std::string("invalid ") + flag + " regex: " + failure.what();
      return false;
    }
  };
  if (!compile(library_regex, library, "lib") ||
      !compile(name_regex, name, "name"))
    return std::nullopt;
  std::vector<LoadedSymbol> symbols;
  if (undefined_only)
    native_undefined_symbols(library, name, symbols, include_system);
  else
    native_loaded_symbols(library, name, symbols, include_system);
  const auto declarations =
      runtime_symbol_declarations(interp_->getASTContext());
  std::map<std::string, std::string> kinds;
  for (const auto& decl : declarations) kinds.emplace(decl.mangled, decl.kind);
  for (auto& symbol : symbols) {
    const auto mangled = symbol.mangled.rfind("__Z", 0) == 0
                             ? symbol.mangled.substr(1)
                             : symbol.mangled;
    if (const auto kind = kinds.find(mangled); kind != kinds.end())
      symbol.kind = kind->second;
  }
  if (std::regex_search(std::string("<session>"), library)) {
    for (const auto& decl : declarations) {
      if (decl.defined == undefined_only) continue;
      const auto readable = unversioned_name(readable_symbol_name(decl.mangled));
      if (!std::regex_search(readable, name)) continue;
      auto address = lookup_symbol(jit_, decl.mangled);
      if (undefined_only) {
        if (address && address->getValue()) continue;
        if (!address) llvm::consumeError(address.takeError());
        symbols.push_back({"<session>", decl.kind, readable, decl.mangled});
        continue;
      }
      if (!address) {
        llvm::consumeError(address.takeError());
        continue;
      }
      Dl_info info{};
      if (dladdr(address->toPtr<void*>(), &info) && info.dli_fbase) continue;
      symbols.push_back({"<session>", decl.kind, readable, decl.mangled});
    }
  }
  auto result = format_loaded_symbols(std::move(symbols));
  return result;
}

std::optional<std::string> CppInterpreter::evaluate(const std::string& code) {
  clang::Value value;
  if (!run(code, value)) return std::nullopt;
  std::string printed;
  if (value.hasValue()) {
    llvm::raw_string_ostream os(printed);
    print_value(value, os);
  }
  // An object value runs its destructor here, while its code is still loaded.
  value.clear();
  return printed;
}

std::string CppInterpreter::load_library(const std::string& path) {
#ifdef __linux__
  // Resolve ELF PLT/GOT entries before native caller matching, even when this
  // API is used directly instead of going through LibraryManager's RTLD_NOW.
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    const char* error = dlerror();
    return error ? error : "cannot load library";
  }
#endif
  if (llvm::Error e = interp_->LoadDynamicLibrary(path.c_str())) {
#ifdef __linux__
    dlclose(handle);
#endif
    return llvm::toString(std::move(e));
  }
#ifdef __linux__
  dlclose(handle);  // The interpreter retains its own permanent library handle.
#endif
  return "";
}

std::vector<SemanticToken> CppInterpreter::semantic_tokens(const std::string& code) {
  const auto* context = interp_->getASTContext().getTranslationUnitDecl();
  if (semantic_context_ == context && semantic_code_ == code && semantic_args_ == args_) {
    return semantic_cache_;
  }
  auto compiler = make_compiler(args_, false);
  if (!compiler) {
    llvm::consumeError(compiler.takeError());
    return {};
  }
  auto result = analyze_editor(std::move(*compiler), *interp_->getCompilerInstance(), code, false);
  semantic_context_ = context;
  semantic_code_ = code;
  semantic_args_ = args_;
  semantic_cache_ = std::move(result.tokens);
  return semantic_cache_;
}

CppInterpreter::Completions CppInterpreter::complete(const std::string& code) {
  const auto* context = interp_->getASTContext().getTranslationUnitDecl();
  if (completion_context_ == context && completion_code_ == code && completion_args_ == args_) {
    return completion_cache_;
  }
  Completions out;
  out.prefix = std::string(trailing_identifier(code));
  auto compiler = make_compiler(args_, false);
  if (!compiler) {
    llvm::consumeError(compiler.takeError());
    return out;
  }
  auto analysis = analyze_editor(std::move(*compiler), *interp_->getCompilerInstance(), code, true);
  out.items = std::move(analysis.items);
  out.signatures = std::move(analysis.signatures);
  out.active_parameter = analysis.active_parameter;
  std::set<std::string> names;
  for (const auto& item : out.items) {
    names.insert(item.text);
  }
  out.names.assign(names.begin(), names.end());
  completion_context_ = context;
  completion_code_ = code;
  completion_args_ = args_;
  completion_cache_ = out;
  return out;
}

CppInterpreter::AddResult CppInterpreter::add_source(const std::string& path) {
  return load_source(path, false);
}

CppInterpreter::AddResult CppInterpreter::reload_source(const std::string& path) {
  return load_source(path, true);
}

void CppInterpreter::reload_changed_sources() {
  for (const auto& path : source_reload_->changed_sources()) {
    auto result = reload_source(path);
    if (!result.ok) llvm::errs() << "error: " << result.message << '\n';
  }
}

CppInterpreter::AddResult CppInterpreter::load_source(const std::string& path, bool reload) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path file = fs::canonical(expand_home(path), ec);
  if (ec) return {false, "no such file: " + path};
  if (!fs::is_regular_file(file, ec)) return {false, "not a regular file: " + path};
  const std::string name = file.string();
  if (name.find_first_of("\"\n") != std::string::npos) {
    return {false, "cannot #include a file name with '\"' or a newline: " + name};
  }
  const bool added = std::find(sources_.begin(), sources_.end(), name) != sources_.end();
  if (added && !reload) return {false, "already added: " + name};
  if (!added && reload) return {false, "source has not been added: " + name};
  llvm::SaveAndRestore<std::string> source_scope(source_input_, name);
  llvm::SaveAndRestore<bool> reload_scope(reloading_source_, reload);
  const auto options = source_compile_options(name);
  const auto include = "#include \"" + name + "\"\n";
  if (!options.source.empty() || file_options_.count(name) ||
      global_options_changed_) {
    std::string error;
    if (!with_compile_options(options, [&] { return execute(include); }, error))
      return {false,
              "could not add " + name + (error.empty() ? "" : ": " + error)};
  } else if (!execute(include))
    return {false, "could not add " + name};
  if (!reload) sources_.push_back(name);
  return {true, std::string(reload ? "reloaded: " : "added: ") + name +
                    (options.source.empty()
                         ? ""
                         : " (compile options: " + options.source + ")")};
}

const std::string& CppInterpreter::compilation_database_path() const {
  return database_->path();
}

SourceCompileOptions CppInterpreter::source_compile_options(
    const std::string& file, const FileCompileOptions* candidate,
    const std::vector<std::string>* globals) const {
  std::error_code ec;
  SourceCompileOptions options;
  options.directory = std::filesystem::current_path(ec).string();
  if (!globals && !reference_directory_.empty())
    options.directory = reference_directory_;
  if (!file.empty()) {
    if (!candidate) {
      if (const auto found = file_options_.find(file);
          found != file_options_.end())
        candidate = &found->second;
    }
    if (!candidate || !candidate->replace_database) {
      if (const auto database = database_->options_for(file))
        options = *database;
    }
  }
  const auto& global_flags = globals ? *globals : args_;
  options.arguments.insert(options.arguments.end(), global_flags.begin(),
                           global_flags.end());
  if (candidate)
    options.arguments.insert(options.arguments.end(), candidate->flags.begin(),
                             candidate->flags.end());
  return options;
}

bool CppInterpreter::with_compile_options(const SourceCompileOptions& options,
                                          const std::function<bool()>& action,
                                          std::string& error) {
  auto configured = make_compiler(options.arguments);
  if (!configured) {
    error = llvm::toString(configured.takeError());
    return false;
  }
  SourceCompileScope scope(*interp_->getCompilerInstance(), **configured,
                           options.directory);
  if (!scope.error().empty()) {
    error = scope.error();
    return false;
  }
  llvm::SaveAndRestore<bool> active(compile_scope_active_, true);
  clang::Value ignored;
  const bool prepared =
      scope.prelude().empty() || run_raw(scope.prelude(), ignored);
  const bool included = prepared && (scope.includes().empty() ||
                                     run_raw(scope.includes(), ignored));
  const bool result = included && action();
  // Failed parsing does not undo macro directives, so restore them separately.
  const bool restored =
      scope.epilogue().empty() || run_raw(scope.epilogue(), ignored);
  ignored.clear();
  if (!prepared || !included)
    error = "could not apply compiler flags or forced includes";
  if (!restored) error = "could not restore compiler macros";
  return result && restored;
}

bool CppInterpreter::add_compile_options(const std::vector<std::string>& flags,
                                         std::string& error,
                                         const std::string& file) {
  return update_compile_options(flags, false, file, error);
}

bool CppInterpreter::set_compile_options(const std::vector<std::string>& flags,
                                         std::string& error,
                                         const std::string& file) {
  return update_compile_options(flags, true, file, error);
}

bool CppInterpreter::use_compile_options(const std::string& file,
                                         std::string& error) {
  error.clear();
  if (file.empty()) {
    error = "missing reference filename";
    return false;
  }
  const auto target = compile_file_path(file, error);
  if (target.empty()) return false;
  std::error_code ec;
  if (!file_options_.count(target) && !database_->options_for(target) &&
      !std::filesystem::is_regular_file(target, ec)) {
    error = "no source file or compile options for: " + file;
    return false;
  }
  // Re-selecting a reference rebases its database flags instead of carrying
  // flags from the previous reference into the new one. Manual globals remain.
  auto globals = reference_file_.empty() ? args_ : reference_global_overrides_;
  const auto options = source_compile_options(target, nullptr, &globals);
  // Capture relative database paths before promoting the options to globals.
  const auto flags =
      normalize_compile_flags(options.arguments, options.directory, error);
  if (!flags || !set_compile_options(*flags, error)) return false;
  reference_file_ = target;
  reference_directory_ = options.directory;
  reference_global_overrides_ = std::move(globals);
  return true;
}

bool CppInterpreter::update_compile_options(
    const std::vector<std::string>& flags, bool replace,
    const std::string& file, std::string& error) {
  error.clear();
  std::error_code ec;
  const auto directory = std::filesystem::current_path(ec).string();
  if (ec) {
    error = ec.message();
    return false;
  }
  const auto normalized = normalize_compile_flags(flags, directory, error);
  if (!normalized) return false;
  std::string target;
  FileCompileOptions candidate;
  SourceCompileOptions options;
  if (file.empty()) {
    options.directory = directory;
    if (!replace) options.arguments = args_;
    options.arguments.insert(options.arguments.end(), normalized->begin(),
                             normalized->end());
  } else {
    target = compile_file_path(file, error);
    if (target.empty()) return false;
    if (!replace) {
      if (const auto found = file_options_.find(target);
          found != file_options_.end())
        candidate = found->second;
    } else
      candidate.replace_database = true;
    candidate.flags.insert(candidate.flags.end(), normalized->begin(),
                           normalized->end());
    options = source_compile_options(target, &candidate);
  }
  // Construct and validate a disposable frontend; do not replay session code or
  // run forced-include initializers while merely changing configuration.
  auto configured = make_compiler(options.arguments);
  if (!configured) {
    error = llvm::toString(configured.takeError());
    return false;
  }
  SourceCompileScope validation(*interp_->getCompilerInstance(), **configured,
                                options.directory);
  if (!validation.error().empty()) {
    error = validation.error();
    return false;
  }
  if (file.empty()) {
    args_ = std::move(options.arguments);
    global_options_changed_ = true;
    if (replace) {
      reference_file_.clear();
      reference_directory_.clear();
      reference_global_overrides_.clear();
    } else if (!reference_file_.empty())
      reference_global_overrides_.insert(reference_global_overrides_.end(),
                                         normalized->begin(),
                                         normalized->end());
  } else
    file_options_[target] = std::move(candidate);
  return true;
}

std::optional<std::string> CppInterpreter::describe_compile_options(
    std::string& error, const std::string& file) const {
  error.clear();
  const auto target = file.empty() ? "" : compile_file_path(file, error);
  if (!file.empty() && target.empty()) return std::nullopt;
  const auto options = source_compile_options(target);
  std::ostringstream out;
  out << "Compile options:\n";
  auto row = [&](const char* label, const std::string& value) {
    out << "  " << std::left << std::setw(18) << label << " : " << value
        << '\n';
  };
  row("Scope", target.empty() ? "global" : target);
  row("Database", database_->path().empty() ? "(none)" : database_->path());
  if (!reference_file_.empty()) row("Reference file", reference_file_);
  row("Working directory", options.directory);
  row("Global flags", format_compile_flags(args_));
  if (!target.empty()) {
    const auto override = file_options_.find(target);
    const bool replaced =
        override != file_options_.end() && override->second.replace_database;
    const auto database = replaced ? std::optional<SourceCompileOptions>()
                                   : database_->options_for(target);
    row("Database command", replaced   ? "replaced by file options"
                            : database ? database->source
                                       : "(none)");
    row("Database flags",
        database ? format_compile_flags(database->arguments) : "(none)");
    row("File flags", override == file_options_.end()
                          ? "(none)"
                          : format_compile_flags(override->second.flags));
  }
  row("Effective flags",
      format_compile_flags(compiler_arguments(options.arguments)));
  if (target.empty()) {
    if (file_options_.empty()) row("File overrides", "(none)");
    for (const auto& [path, flags] : file_options_)
      row("File override",
          path + (flags.replace_database ? " [replaced] " : " [appended] ") +
              format_compile_flags(flags.flags));
  }
  row("Applies to",
      "future compilation; existing declarations and objects remain");
  row("Runtime updates",
      "preprocessing and language options; JIT target/code generation stay "
      "fixed");
  return out.str();
}

std::vector<std::string> CppInterpreter::class_names() {
  return user_class_names(interp_->getASTContext());
}

std::optional<std::string> CppInterpreter::describe_class(
    const std::string& name, std::string& error) {
  const clang::CXXRecordDecl* record =
      find_class(interp_->getASTContext(), name);
  if (record == nullptr) {
    error = "no class named '" + name + "' (try %class)";
    return std::nullopt;
  }
  return example::describe_class(interp_->getASTContext(), *record);
}

std::optional<std::string> CppInterpreter::describe_members(const std::string& name,
                                                            MemberKind kind, MemberOptions options,
                                                            std::string& error) {
  error.clear();
  auto& ctx = interp_->getASTContext();
  const auto* record = find_class(ctx, name);
  if (!record) {
    error = "no complete record named '" + name + "' (try %class)";
    return std::nullopt;
  }
  return example::describe_members(ctx, *record, kind, options);
}

std::string CppInterpreter::version() { return "LLVM " LLVM_VERSION_STRING; }

std::vector<std::string> CppInterpreter::symbol_names() {
  return user_symbol_names(interp_->getASTContext());
}

std::optional<std::string> CppInterpreter::symbol_info(const std::string& name,
                                                       std::string& error) {
  error.clear();
  auto address_text = [&](const std::string& symbol,
                          const std::string& unavailable) {
    auto address = lookup_symbol(jit_, symbol);
    if (!address) {
      llvm::consumeError(address.takeError());
      return unavailable;
    }
    std::ostringstream out;
    out << "0x" << std::hex << address->getValue();
    return out.str();
  };
  auto symbols = describe_symbols(interp_->getASTContext(), name);
  // Known session definitions and type-only declarations need no process-wide
  // import scan. Externals and names without an AST declaration still do.
  bool inspect_native = symbols.empty();
  for (const auto& symbol : symbols) {
    if (symbol.mangled.empty()) continue;
    auto address = lookup_symbol(jit_, symbol.mangled);
    if (address) {
      Dl_info info{};
      if (dladdr(address->toPtr<void*>(), &info) && info.dli_fbase) inspect_native = true;
    } else {
      llvm::consumeError(address.takeError());
      const bool defined = std::any_of(symbol.fields.begin(), symbol.fields.end(), [](const auto& field) { return field.first == "Definition" && field.second == "yes"; });
      if (!defined) inspect_native = true;
    }
  }
  auto native = inspect_native ? native_symbol_details(name) : std::vector<SymbolDescription>{};
  const auto declarations = runtime_symbol_declarations(interp_->getASTContext());
  for (auto& symbol : native) {
    const auto declaration = std::find_if(declarations.begin(), declarations.end(), [&](const auto& item) { return item.mangled == symbol.mangled; });
    if (declaration != declarations.end()) for (auto& field : symbol.fields)
      if (field.first == "Kind") field.second = declaration->kind;
  }
  if (symbols.empty() && !native.empty()) symbols = native;
  else for (auto& symbol : symbols) {
    const auto found = std::find_if(native.begin(), native.end(), [&](const auto& item) { return item.mangled == symbol.mangled; });
    if (found == native.end()) continue;
    for (const auto& field : found->fields)
      if (field.first != "Kind" && field.first != "Mangled" && field.first != "Address") symbol.fields.push_back(field);
  }
  if (symbols.empty()) {
    auto address = lookup_symbol(jit_, name);
    if (!address) {
      llvm::consumeError(address.takeError());
      address = interp_->getSymbolAddressFromLinkerName(name);
    }
    if (!address) {
      llvm::consumeError(address.takeError());
      error = "no symbol named '" + name + "'";
      return std::nullopt;
    }
    std::ostringstream value;
    value << "0x" << std::hex << address->getValue();
    SymbolDescription raw;
    raw.name = readable_symbol_name(name);
    raw.mangled = name;
    raw.fields = {{"Kind", "JIT symbol"}, {"Address", value.str()}, {"Defined", "yes (runtime symbol)"},
                  {"Declaration", "unavailable (no AST declaration)"}};
    symbols.push_back(std::move(raw));
  }
  for (auto& symbol : symbols) {
    const auto already_address = std::any_of(symbol.fields.begin(), symbol.fields.end(), [](const auto& field) { return field.first == "Address"; });
    const auto address = symbol.mangled.empty() ? symbol.address_note :
        address_text(symbol.mangled, "unavailable (not emitted or unresolved)");
    if (!already_address) symbol.fields.emplace_back("Address", address);
    if (symbol.mangled.empty()) continue;
    const bool resolved = address.rfind("0x", 0) == 0 || std::any_of(symbol.fields.begin(), symbol.fields.end(), [](const auto& field) {
      return field.first == "Address" && field.second.rfind("0x", 0) == 0;
    });
    symbol.fields.emplace_back("Resolved", resolved ? "yes" : "no (undefined or not emitted)");
    if (std::none_of(symbol.fields.begin(), symbol.fields.end(), [](const auto& field) { return field.first == "Defined"; })) {
      const bool definition = std::any_of(symbol.fields.begin(), symbol.fields.end(), [](const auto& field) { return field.first == "Definition" && field.second == "yes"; });
      symbol.fields.emplace_back("Defined", definition ? "yes (session definition)" : "no (declaration only)");
      symbol.fields.emplace_back("Undefined", !definition && !resolved ? "yes (unresolved declaration)" : "no");
    }
    const auto refs = symbol_reference_sites(interp_->getASTContext(), symbol.mangled);
    symbol.fields.emplace_back("Source refs", std::to_string(refs.size()));
    for (const auto& reference : refs) symbol.fields.emplace_back("Ref source", reference);
    const auto details = hooks_.symbol_details(symbol.mangled);
    if (!details.original_symbol.empty()) {
      symbol.fields.emplace_back("Original IR", details.original_symbol);
      symbol.fields.emplace_back("Original", address_text(details.original_symbol, "unavailable"));
      if (!details.routing_symbol.empty()) {
        symbol.fields.emplace_back("Routing IR", details.routing_symbol);
        symbol.fields.emplace_back("Routing", address_text(details.routing_symbol, "unavailable"));
      }
    }
    if (!details.original_symbol.empty() || !details.hooks.empty()) {
      if (details.hooks.empty()) symbol.fields.emplace_back("Hooks", "none");
      for (const auto& hook : details.hooks) {
        const std::string kind = hook.kind == HookKind::Mock ? "%mock" : hook.kind == HookKind::Pre ? "%pre" : "%post";
        symbol.fields.emplace_back("Hook", kind + (hook.caller.empty() ? " (global)" : " in " + hook.caller));
      }
    }
  }
  return format_symbols(symbols, name);
}

}  // namespace example
