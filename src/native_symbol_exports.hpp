#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace example {

// Publish existing JIT storage to dlopen's symbol namespace. Each generated
// image contains an absolute export, not a second allocation or initializer.
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
