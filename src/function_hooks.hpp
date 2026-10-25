#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
class FunctionDecl;
class Interpreter;
struct PartialTranslationUnit;
}  // namespace clang

namespace llvm::orc {
class LLJIT;
}

namespace example {

enum class HookKind { Mock, Pre, Post };

struct ActiveHook {
  HookKind kind;
  std::string signature;
  std::string body;
  std::string caller;
};

struct HookInstrumentation {
  std::vector<std::string> functions;
  std::vector<std::pair<std::string, std::string>> calls;
};

struct HookSymbolDetails {
  std::string original_symbol;
  std::string routing_symbol;
  std::vector<ActiveHook> hooks;
};

struct CallableDefinition {
  std::string declaration;
  std::string mangled;
  unsigned template_specializations = 0;
  clang::FunctionDecl* function = nullptr;  // Borrowed from the interpreter AST.
};

// Instruments user definitions and references to library functions before JIT
// compilation, so earlier JIT callers and pointers observe later hook changes.
class FunctionHooks {
 public:
  FunctionHooks(clang::Interpreter& interpreter, llvm::orc::LLJIT& jit);
  ~FunctionHooks();
  HookInstrumentation instrument(clang::PartialTranslationUnit& input);
  void discard(const HookInstrumentation& added);
  bool set(HookKind kind, const std::string& signature, const std::string& body, std::string& error,
           const std::string& caller = "");
  // Empty signature restores every hooked target in this session.
  bool restore(const std::string& signature, std::string& error, const std::string& caller = "");
  std::vector<ActiveHook> active() const;
  HookSymbolDetails symbol_details(const std::string& symbol) const;
  // Resolve a declaration and build an actual out-of-line C++ definition.
  std::optional<CallableDefinition> definition(const std::string& signature, std::string& error);

private:
  bool update(std::optional<HookKind> kind, const std::string& signature, const std::string& body,
              std::string& error, const std::string& caller);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace example
