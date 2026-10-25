#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "function_hooks.hpp"

namespace clang {
class Interpreter;
class Value;
}  // namespace clang

namespace example {

class ProjectCompilationDatabase;
class NativeSymbolExports;
struct SourceCompileOptions;

// The clang-repl engine behind the REPL: clang::Interpreter parses each input
// as a partial translation unit on top of the earlier ones, and runs it
// in-process with the ORC JIT. Clang diagnostics go to stderr.
class CppInterpreter {
 public:
  // `args` are extra compiler flags (e.g. -std=c++20, -I<dir>). Returns
  // nullptr and sets `error` on failure.
  static std::unique_ptr<CppInterpreter> create(
      const std::vector<std::string>& args, std::string& error);

  ~CppInterpreter();
  CppInterpreter(const CppInterpreter&) = delete;
  CppInterpreter& operator=(const CppInterpreter&) = delete;

  // Parses and runs one input; a final expression without ';' prints its
  // value, e.g. "(int) 42". Returns false after reporting an error; the
  // session stays usable.
  bool execute(const std::string& code);

  // Empty body removes that hook. A failed update preserves existing hooks.
  bool set_hook(HookKind kind, const std::string& signature, const std::string& body,
                std::string& error, const std::string& caller = "");
  // Removes the mock and both hooks together; empty signature restores the
  // whole session. Repeated restores are harmless.
  bool restore(const std::string& signature, std::string& error, const std::string& caller = "");
  // Active signatures and original C++ bodies, sorted by signature and kind;
  // does not execute code.
  std::vector<ActiveHook> active_hooks() const;

  // Like execute(), but returns the printed value of a final expression
  // without ';' ("" when there is none), or nullopt on error.
  std::optional<std::string> evaluate(const std::string& code);

  struct Completions {
    std::vector<std::string> names;  // sorted; each starts with `prefix`
    std::string prefix;              // the identifier being completed
  };
  // Tab completion at the end of `code` (the text before the cursor): Clang's
  // code completion over the session's declarations (names, members after
  // '.' / '->', std names, keywords) plus the data members of an object.
  Completions complete(const std::string& code);

  // Parses and runs a C++ source file in the session as if it were
  // #included, so diagnostics point into the file and its own #include "..."
  // lines resolve next to it. Compile options come from the startup database,
  // using an exact entry or a C++ entry in the same directory. A file can be
  // added once; existing session state and hooks are preserved.
  struct AddResult {
    bool ok;
    std::string message;
  };
  AddResult add_source(const std::string& path);
  const std::vector<std::string>& sources() const { return sources_; }
  const std::string& compilation_database_path() const;
  // Empty file selects global flags. File append inherits database/global
  // flags; file replacement discards its database flags and retains globals.
  // Updates affect future compilation, preserve live state, and are atomic on
  // validation errors. Manual include paths are anchored to the current cwd.
  bool add_compile_options(const std::vector<std::string>& flags,
                           std::string& error, const std::string& file = "");
  bool set_compile_options(const std::vector<std::string>& flags,
                           std::string& error, const std::string& file = "");
  bool use_compile_options(const std::string& file, std::string& error);
  std::optional<std::string> describe_compile_options(
      std::string& error, const std::string& file = "") const;

  // Classes defined by user code (inputs, added sources and their non-system
  // headers; qualified names), and a synopsis of one:
  // bases, members, size and field offsets. describe_class() returns nullopt
  // and sets `error` if `name` is not a class.
  std::vector<std::string> class_names();
  std::optional<std::string> describe_class(const std::string& name, std::string& error);
  // Symbol metadata (all overloads for a name) and available JIT addresses.
  // Does not parse/evaluate user expressions or call the inspected symbol.
  std::optional<std::string> symbol_info(const std::string& name, std::string& error);
  std::vector<std::string> symbol_names();
  // Define missing storage and publish it to the native loader before %lib.
  // Existing session/library definitions are never replaced.
  bool define_variable(const std::string& name, const std::string& initializer,
                        std::string& error);
  bool define_callable(const std::string& signature, const std::string& body, std::string& error);
  // Defined native exports and emitted session functions/globals. Regex filters
  // search full library paths (or <session>) and demangled qualified names.
  // System libraries are hidden unless include_system is true.
  std::optional<std::string> symbol_list(const std::string& library_regex,
                                       const std::string& name_regex, std::string& error,
                                       bool include_system = false,
                                       bool undefined_only = false);

  // Makes the symbols of a shared library (already dlopen-ed or not) visible
  // to JIT-ed code. Returns an error message, or "" on success.
  std::string load_library(const std::string& path);

  // "LLVM <version>" of the linked Clang.
  static std::string version();

 private:
  CppInterpreter(std::unique_ptr<clang::Interpreter> interp, std::vector<std::string> args,
                 std::unique_ptr<ProjectCompilationDatabase> database);
  bool run(const std::string& code, clang::Value& value);
  bool run_raw(const std::string& code, clang::Value& value);
  bool with_compile_options(const SourceCompileOptions& options,
                            const std::function<bool()>& action,
                            std::string& error);
  bool update_compile_options(const std::vector<std::string>& flags,
                              bool replace, const std::string& file,
                              std::string& error);

  struct FileCompileOptions {
    bool replace_database = false;
    std::vector<std::string> flags;
  };
  SourceCompileOptions source_compile_options(
      const std::string& file, const FileCompileOptions* candidate = nullptr,
      const std::vector<std::string>* globals = nullptr) const;

  // Keep absolute exports alive while the JIT destroys its global objects.
  std::unique_ptr<NativeSymbolExports> native_exports_;
  std::unique_ptr<clang::Interpreter> interp_;
  FunctionHooks hooks_;
  std::vector<std::string> args_;  // the user's compiler flags, for completion
  std::vector<std::string> sources_;  // canonical paths of the %add-ed files
  std::unique_ptr<ProjectCompilationDatabase> database_;
  std::map<std::string, FileCompileOptions> file_options_;
  std::string reference_file_;
  std::string reference_directory_;
  std::vector<std::string> reference_global_overrides_;
  bool global_options_changed_ = false;
  bool compile_scope_active_ = false;
};

}  // namespace example
