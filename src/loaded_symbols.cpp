#include "loaded_symbols.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <tuple>

#include "llvm/Demangle/Demangle.h"

#ifdef CPP_REPL_RUNTIME_SYMBOLS
#include "gum_runtime.hpp"
#endif

namespace example {
namespace {

std::string one_line(const std::string& text) {
  std::string result;
  for (const unsigned char c : text) {
    if (std::iscntrl(c)) {
      result += ' ';
    } else {
      result += static_cast<char>(c);
    }
  }
  return result;
}

} // namespace

std::string readable_symbol_name(const std::string& mangled) {
  // Some Mach-O symbol sources retain the extra platform underscore.
  const auto name = mangled.rfind("__Z", 0) == 0 ? mangled.substr(1) : mangled;
  return llvm::demangle(name);
}

bool is_system_library(const std::string& path) {
  // Include platform runtimes and system-wide Homebrew toolchain libraries.
  for (const auto* root : {"/System/", "/Library/Apple/", "/usr/lib/", "/usr/lib64/",
                           "/lib/", "/lib64/", "/opt/homebrew/", "/usr/local/Cellar/",
                           "/usr/local/opt/"}) {
    if (path.rfind(root, 0) == 0) return true;
  }
  return path == "linux-vdso.so.1" || path == "linux-gate.so.1" || path == "[vdso]";
}

void native_loaded_symbols(const std::regex& library, const std::regex& name,
                           std::vector<LoadedSymbol>& result, bool include_system) {
#ifdef CPP_REPL_RUNTIME_SYMBOLS
  initialize_gum();

  struct Query {
    const std::regex& library;
    const std::regex& name;
    std::vector<LoadedSymbol>& result;
    bool include_system;
  } query{library, name, result, include_system};

  gum_process_enumerate_modules(
      [](GumModule* module, gpointer data) -> gboolean {
        auto& query = *static_cast<Query*>(data);
        const std::string path = gum_module_get_path(module);
        if ((!query.include_system && is_system_library(path)) ||
            !std::regex_search(path, query.library)) {
          return TRUE;
        }
        struct Exports {
          Query& query;
          const std::string& path;
        } exports{query, path};
        gum_module_enumerate_exports(
            module,
            [](const GumExportDetails* details, gpointer data) -> gboolean {
              auto& exports = *static_cast<Exports*>(data);
              if (!details->name || !*details->name || !details->address) {
                return TRUE;
              }
              const std::string name = readable_symbol_name(details->name);
              if (!std::regex_search(name, exports.query.name)) {
                return TRUE;
              }
              std::string kind = details->type == GUM_EXPORT_FUNCTION ? "function" : "variable";
              for (const auto* prefix : {"vtable for ", "VTT for ", "typeinfo for ",
                                         "typeinfo name for ", "guard variable for "}) {
                if (name.rfind(prefix, 0) == 0) {
                  kind = std::string(prefix).substr(0, std::string(prefix).find(" for "));
                  break;
                }
              }
              exports.query.result.push_back({exports.path, kind, name, details->name, details->address});
              return TRUE;
            },
            &exports);
        return TRUE;
      },
      &query);
#else
  (void)library;
  (void)name;
  (void)result;
  (void)include_system;
#endif
}

std::string format_loaded_symbols(std::vector<LoadedSymbol> symbols) {
  std::vector<std::string> names;
  for (const auto& symbol : symbols) names.push_back(one_line(symbol.name));
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  std::string result;
  for (const auto& name : names) result += name + '\n';
  return result;
}

} // namespace example
