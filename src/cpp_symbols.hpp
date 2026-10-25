#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clang {
class ASTContext;
}

namespace example {

struct SymbolDescription {
  std::string name;
  std::string mangled;
  std::string address_note = "not applicable";
  std::vector<std::pair<std::string, std::string>> fields;
};

struct RuntimeDeclaration {
  std::string mangled;
  std::string kind;
  bool defined = false;
};

struct VariableDefinition {
  std::string declaration;
  std::string mangled;
  bool defined = false;
  bool thread_local_storage = false;
};

// Build a declarator from the AST's actual type, including array/pointer
// syntax.
std::optional<VariableDefinition> variable_definition(
    clang::ASTContext& context, std::string_view name, std::string& error);
// Called only after a failed definition of a previously declaration-only name.
void discard_failed_variable_definition(clang::ASTContext& context,
                                        std::string_view name);

// User function/global declarations for native kind refinement and JIT listing.
std::vector<RuntimeDeclaration> runtime_symbol_declarations(
    clang::ASTContext& context);
std::vector<std::string> symbol_reference_sites(clang::ASTContext& context,
                                                const std::string& mangled);

// Read declarations without parsing or executing an expression. A name can
// identify several overloads; a mangled name identifies a single declaration.
std::vector<SymbolDescription> describe_symbols(clang::ASTContext& context,
                                                std::string_view name);
std::vector<std::string> user_symbol_names(clang::ASTContext& context);
std::string format_symbols(const std::vector<SymbolDescription>& symbols,
                           const std::string& query);

}  // namespace example
