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

// Validate flag/value grouping with Clang's option table, and anchor manual
// include paths to the directory where the user supplied them.
std::optional<std::vector<std::string>> normalize_compile_flags(
    const std::vector<std::string>& flags, const std::string& directory,
    std::string& error);

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
