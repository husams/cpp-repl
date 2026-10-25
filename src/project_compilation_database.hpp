#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace example {

struct SourceCompileOptions {
  std::vector<std::string> arguments;
  std::string directory;
  std::string source;
};

class ProjectCompilationDatabase {
 public:
  ProjectCompilationDatabase();
  ~ProjectCompilationDatabase();
  static std::unique_ptr<ProjectCompilationDatabase> load(std::string& error);
  std::optional<SourceCompileOptions> options_for(
      const std::string& file) const;
  const std::string& path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace example
