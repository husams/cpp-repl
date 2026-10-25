#pragma once

#include <string>
#include <vector>

namespace clang {
class CompilerInstance;
class NamedDecl;
} // namespace clang

namespace example {
void enable_definition_shadowing(clang::CompilerInstance &compiler);
std::string unversioned_name(std::string name);
bool is_current_definition(const clang::NamedDecl &declaration);
void version_source_files(clang::CompilerInstance &compiler,
                          const std::vector<std::string> &files);
void hide_source_definitions(clang::CompilerInstance &compiler,
                             const std::vector<std::string> &files);

// A rejected input must put every shadowed declaration back into lookup.
class DefinitionTransaction {
public:
  explicit DefinitionTransaction(clang::CompilerInstance &compiler);
  ~DefinitionTransaction();
  void commit();
  DefinitionTransaction(const DefinitionTransaction &) = delete;
  DefinitionTransaction &operator=(const DefinitionTransaction &) = delete;

private:
  clang::CompilerInstance &compiler_;
  bool committed_ = false;
};
} // namespace example
