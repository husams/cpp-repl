#include "native_symbol_exports.hpp"

#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#ifndef CPP_REPL_CLANG_EXECUTABLE
#define CPP_REPL_CLANG_EXECUTABLE "clang"
#endif

namespace example {

NativeSymbolExports::~NativeSymbolExports() {
  for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) dlclose(*it);
  if (!directory_.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
}

bool NativeSymbolExports::prepare(std::string& error) {
  if (!directory_.empty()) return true;
  const auto compiler = llvm::sys::findProgramByName(CPP_REPL_CLANG_EXECUTABLE);
  if (!compiler) {
    error = "cannot find Clang to publish native symbols: " +
            compiler.getError().message();
    return false;
  }
  llvm::SmallString<128> directory;
  std::error_code temporary_error;
  const auto temporary = std::filesystem::temp_directory_path(temporary_error);
  if (temporary_error) {
    error = "cannot locate native symbol temporary directory: " +
            temporary_error.message();
    return false;
  }
  if (const auto failure = llvm::sys::fs::createUniqueDirectory(
          (temporary / "cpp-repl-symbols").string(), directory)) {
    error = "cannot create native symbol directory: " + failure.message();
    return false;
  }
  directory_ = directory.str().str();
  std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               temporary_error);
  if (temporary_error) {
    error =
        "cannot protect native symbol directory: " + temporary_error.message();
    std::error_code ignored;
    std::filesystem::remove(directory_, ignored);
    directory_.clear();
    return false;
  }
  compiler_ = *compiler;
  return true;
}

bool NativeSymbolExports::publish(const std::string& mangled,
                                  std::uintptr_t address, std::string& error) {
  return publish({{mangled, address}}, error);
}

bool NativeSymbolExports::publish(
    const std::vector<std::pair<std::string, std::uintptr_t>>& symbols, std::string& error) {
  if (!prepare(error)) return false;
  for (const auto& [mangled, address] : symbols) {
    if (mangled.empty() || mangled.find_first_of("\n\r\0", 0, 3) != std::string::npos) {
      error = "unsupported native symbol spelling";
      return false;
    }
    if (auto* existing = dlsym(RTLD_DEFAULT, mangled.c_str());
        existing && reinterpret_cast<std::uintptr_t>(existing) != address) {
      error = "native symbol already resolves to a different address: " + mangled;
      return false;
    }
  }
  const auto stem = directory_ + "/storage-" + std::to_string(next_++);
  const auto assembly = stem + ".s";
  const auto diagnostics = stem + ".log";
#ifdef __APPLE__
  const auto image = stem + ".dylib";
#else
  const auto image = stem + ".so";
#endif
  {
    std::ofstream out(assembly);
    for (const auto& [mangled, address] : symbols) {
#ifdef __APPLE__
      const auto native_name = "_" + mangled;
#else
      const auto& native_name = mangled;
#endif
      out << ".globl " << std::quoted(native_name) << "\n.set " << std::quoted(native_name)
          << ", 0x" << std::hex << address << '\n';
    }
#ifdef __linux__
    out << ".section .note.GNU-stack,\"\",@progbits\n";
#endif
    out.close();
    if (!out) {
      error = "cannot write native symbol export";
      return false;
    }
  }
  // No shell: symbol spellings and temporary paths are data, not commands.
  const llvm::StringRef arguments[] = {compiler_,
#ifdef __APPLE__
                                       "-dynamiclib",
#else
                                       "-shared", "-nostdlib",
#endif
                                       "-x",          "assembler", assembly,
                                       "-o",          image};
  const std::optional<llvm::StringRef> redirects[] = {
      llvm::StringRef(""), llvm::StringRef(""), diagnostics};
  std::string invocation_error;
  const int status = llvm::sys::ExecuteAndWait(
      compiler_, arguments, std::nullopt, redirects, 30, 0, &invocation_error);
  if (status != 0) {
    std::ifstream input(diagnostics);
    const std::string details((std::istreambuf_iterator<char>(input)), {});
    error = "Clang could not publish native symbol: " + invocation_error + details;
    return false;
  }
  void* handle = dlopen(image.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    const char* message = dlerror();
    error = "cannot publish native symbol: " + std::string(message ? message : "dlopen failed");
    return false;
  }
  for (const auto& [mangled, address] : symbols) {
    if (reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, mangled.c_str())) != address) {
      dlclose(handle);
      error = "native loader did not publish the JIT symbol address: " + mangled;
      return false;
    }
  }
  handles_.push_back(handle);
  return true;
}

}  // namespace example
