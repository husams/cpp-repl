#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace example {

// Owns only the selected caller's direct branch instructions. The library on
// disk and the target function's entry point are never changed.
class NativeCallPatch {
  struct Impl;

  struct ConstructionKey {};

public:
  NativeCallPatch(ConstructionKey, std::unique_ptr<Impl> impl);
  static bool is_native_address(std::uintptr_t address);
  // Read-only scan using the same supported instruction matching as hooks.
  static std::vector<std::uintptr_t> reference_sites(std::uintptr_t caller, std::uintptr_t callee);
  static std::shared_ptr<NativeCallPatch> prepare(std::uintptr_t caller, std::uintptr_t callee,
                                                  std::string& error);
  ~NativeCallPatch();
  bool install(std::uintptr_t destination, std::string& error);
  bool restore(std::string& error);
  bool active() const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace example
