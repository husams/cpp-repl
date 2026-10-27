#pragma once

#include <memory>
#include <string>
#include <vector>

namespace example {

// Replaces this process with LLDB; returns only when starting LLDB fails.
int launch_debugger(const char* executable, const std::vector<std::string>& flags);

// Keep prompt inputs available to the debugger until interpreter shutdown.
class DebugSources {
  struct FactoryKey {};

 public:
  DebugSources(FactoryKey, std::string directory);
  static std::unique_ptr<DebugSources> create(std::string& error);
  ~DebugSources();
  bool prepare(const std::string& code, std::string& input, std::string& error);
  const std::string& directory() const { return directory_; }

 private:
  std::string directory_;
  unsigned next_ = 0;
};

}  // namespace example
