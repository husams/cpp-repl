#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
class Interpreter;
class FunctionDecl;
struct PartialTranslationUnit;
} // namespace clang

namespace llvm::orc {
class LLJIT;
}

namespace example {
class FunctionHooks;
class DefinitionTransaction;

struct PreparedDefinitions {
  PreparedDefinitions();
  ~PreparedDefinitions();
  std::string path;
  std::string context;
  bool reload = false;
  std::unique_ptr<DefinitionTransaction> transaction;
  std::vector<clang::PartialTranslationUnit*> inputs;
  std::size_t source_inputs = 0;
  std::vector<clang::FunctionDecl*> callables;
  // Instances requested by the prepared input are temporarily reclassified
  // while an explicit missing definition is supplied. Restore on cancellation.
  std::vector<std::pair<clang::FunctionDecl*, unsigned>> template_instances;
  std::vector<std::string> variables;
  std::vector<std::string> exports;
  bool execution_attempted = false;
};

// Fork the single-threaded REPL so parser/preprocessor changes, including
// failed analysis, cannot affect the parent. The callback must never execute C++.
std::optional<std::string>
isolated_definition_scan(const std::function<std::optional<std::string>(std::string&)>& scan,
                         std::string& error);
std::string generate_definition_entries(clang::Interpreter& interpreter, FunctionHooks& hooks,
                                        llvm::orc::LLJIT& jit, const PreparedDefinitions& prepared,
                                        const std::function<bool(const std::string&, bool,
                                                                 const std::string&)>& can_define);
bool write_definition_script(const std::string& path, const std::string& script, bool update,
                             std::string& error);
} // namespace example
