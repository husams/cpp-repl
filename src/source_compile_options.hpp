#pragma once

#include <memory>
#include <string>

namespace clang {
class CompilerInstance;
}

namespace example {

// Temporarily configures the persistent frontend without replacing its AST,
// JIT, globals, or hooks. Macro changes are applied through normal PP input.
class SourceCompileScope {
 public:
  SourceCompileScope(clang::CompilerInstance& session,
                     clang::CompilerInstance& configured,
                     const std::string& directory);
  ~SourceCompileScope();
  SourceCompileScope(const SourceCompileScope&) = delete;
  SourceCompileScope& operator=(const SourceCompileScope&) = delete;

  const std::string& error() const;
  const std::string& prelude() const;
  const std::string& includes() const;
  const std::string& epilogue() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace example
