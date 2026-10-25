#pragma once

#include <memory>
#include <string>
#include <vector>

namespace clang {
class CompilerInstance;
}

namespace example {
class SourceReload {
public:
  explicit SourceReload(clang::CompilerInstance &compiler);
  ~SourceReload();
  bool begin(const std::string &root, bool reload, std::string &error);
  void finish(bool committed);
  std::vector<std::string> changed_sources() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace example
