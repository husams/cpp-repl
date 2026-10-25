#include "loaded_symbols.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <tuple>

#include "llvm/Demangle/Demangle.h"

#ifdef CPP_REPL_RUNTIME_SYMBOLS
#include "native_runtime.hpp"
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

// Directories of the running C runtime and dynamic loader. A custom toolchain
// may ship them outside the standard host locations.
const std::vector<std::string>& runtime_directories() {
  static const std::vector<std::string> directories = [] {
    std::vector<std::string> result;
    for (const auto* symbol : {"malloc", "_r_debug"}) {
      Dl_info info{};
      void* address = dlsym(RTLD_DEFAULT, symbol);
      if (!address || !dladdr(address, &info) || !info.dli_fname) continue;
      const std::string path = info.dli_fname;
      const auto slash = path.rfind('/');
      if (slash == std::string::npos || slash == 0) continue;
      const auto directory = path.substr(0, slash + 1);
      if (std::find(result.begin(), result.end(), directory) == result.end())
        result.push_back(directory);
    }
    return result;
  }();
  return directories;
}

}  // namespace

std::string readable_symbol_name(const std::string& mangled) {
  // Some Mach-O symbol sources retain the extra platform underscore.
  const auto name = mangled.rfind("__Z", 0) == 0 ? mangled.substr(1) : mangled;
  return llvm::demangle(name);
}

bool is_system_library(const std::string& path) {
  // Include platform runtimes and system-wide Homebrew toolchain libraries.
  for (const auto* root :
       {"/System/", "/Library/Apple/", "/usr/lib/", "/usr/lib64/", "/lib/",
        "/lib64/", "/opt/homebrew/", "/usr/local/Cellar/", "/usr/local/opt/"}) {
    if (path.rfind(root, 0) == 0) return true;
  }
  for (const auto& directory : runtime_directories()) {
    if (path.rfind(directory, 0) == 0) return true;
  }
  return path == "linux-vdso.so.1" || path == "linux-gate.so.1" ||
         path == "[vdso]";
}

void native_loaded_symbols(const std::regex& library, const std::regex& name,
                           std::vector<LoadedSymbol>& result,
                           bool include_system) {
#ifdef CPP_REPL_RUNTIME_SYMBOLS
  for (const auto& module : native_modules()) {
    if ((!include_system && is_system_library(module.path)) ||
        !std::regex_search(module.path, library))
      continue;
    for (const auto& symbol : native_exports(module)) {
      const auto readable = readable_symbol_name(symbol.name);
      if (!std::regex_search(readable, name)) continue;
      std::string kind = symbol.function ? "function" : "variable";
      for (const auto* prefix : {"vtable for ", "VTT for ", "typeinfo for ",
                                 "typeinfo name for ", "guard variable for "}) {
        if (readable.rfind(prefix, 0) == 0) {
          kind =
              std::string(prefix).substr(0, std::string(prefix).find(" for "));
          break;
        }
      }
      result.push_back(
          {module.path, kind, readable, symbol.name, symbol.address});
    }
  }
#else
  (void)library;
  (void)name;
  (void)result;
  (void)include_system;
#endif
}

void native_undefined_symbols(const std::regex& library, const std::regex& name,
                              std::vector<LoadedSymbol>& result,
                              bool include_system) {
#ifdef CPP_REPL_RUNTIME_SYMBOLS
  for (const auto& module : native_modules()) {
    if ((!include_system && is_system_library(module.path)) ||
        !std::regex_search(module.path, library))
      continue;
    for (const auto& symbol :
         native_imports(module, [&](const std::string& mangled) {
           return std::regex_search(readable_symbol_name(mangled), name);
         })) {
      if (!symbol.address)
        result.push_back({module.path, symbol.kind,
                          readable_symbol_name(symbol.name), symbol.name});
    }
  }
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

}  // namespace example
