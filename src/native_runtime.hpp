#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace example {

struct NativeRange {
  std::uintptr_t address;
  std::size_t size;
  int protection;
};

struct NativeModule {
  std::string name, path, version = "not recorded";
  std::uintptr_t base = 0, bias = 0;
  std::size_t size = 0;
  bool main = false;
  std::vector<std::string> dependencies;
  std::vector<NativeRange> ranges;
};

struct NativeExport {
  std::string name;
  std::uintptr_t address;
  bool function;
};

struct NativeImport {
  std::string name, kind;
  std::uintptr_t address = 0, slot = 0;
  bool weak = false;
};

std::vector<NativeModule> native_modules();
std::vector<NativeExport> native_exports(const NativeModule& module);
std::vector<NativeImport> native_imports(const NativeModule& module,
                                         const std::function<bool(const std::string&)>& matches);
bool native_function_has_aliases(std::uintptr_t address);
std::uintptr_t native_code_address(std::uintptr_t address);

bool memory_readable(const void* address, std::size_t size);
std::size_t code_page_size();
void* allocate_code_near(std::uintptr_t address, std::size_t distance);
bool make_code_executable(void* address);
void free_code(void* address);
bool patch_code(std::uintptr_t address, const void* bytes, std::size_t size);

} // namespace example
