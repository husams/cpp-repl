#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace example {

// Publish JIT data or code addresses to dlopen's symbol namespace. Each
// generated image contains only an absolute export to the existing address.
class NativeSymbolExports {
 public:
  NativeSymbolExports() = default;
  ~NativeSymbolExports();
  NativeSymbolExports(const NativeSymbolExports&) = delete;
  NativeSymbolExports& operator=(const NativeSymbolExports&) = delete;
  bool prepare(std::string& error);
  bool publish(const std::string& mangled, std::uintptr_t address,
               std::string& error);

 private:
  std::string directory_;
  std::string compiler_;
  std::vector<void*> handles_;
  unsigned next_ = 0;
};

}  // namespace example
