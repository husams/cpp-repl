#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "editor_pch.hpp"

namespace clang {
class CompilerInstance;
}

namespace example {

enum class SemanticKind {
  Variable,
  Parameter,
  Field,
  Function,
  Method,
  Type,
  Namespace,
  EnumConstant,
  Macro,
  Keyword
};

struct CompletionParameter {
  std::string name;
  std::string type;
  bool has_default = false;
  bool is_pack = false;
};

struct CompletionItem {
  std::string text;   // Only this text is inserted into the buffer.
  std::string label;  // Name and parameters, including overloads.
  std::string identity; // Stable qualified callable signature key.
  std::string detail; // Type or return type.
  std::string documentation;
  SemanticKind kind = SemanticKind::Variable;
  unsigned priority = 50; // Clang priority; smaller values rank first.
  bool is_header_path = false;
  bool insert_call = true;
  std::vector<CompletionParameter> parameters;
  bool variadic = false;
};

struct SemanticToken {
  std::size_t begin = 0; // Half-open UTF-8 byte offsets in the edited input.
  std::size_t end = 0;
  SemanticKind kind = SemanticKind::Variable;
};

struct EditorAnalysis {
  bool pch_loaded = false;
  std::string pch_error;
  std::vector<CompletionItem> items;
  std::vector<std::string> signatures;
  std::vector<CompletionItem> signature_items;
  unsigned active_parameter = 0;
  // Override the ordinary identifier prefix when completing a non-identifier
  // token such as an include path.
  std::optional<std::string> completion_prefix;
  std::vector<SemanticToken> tokens;
};

// Uses a disposable syntax-only compiler; never executes edited input.
EditorAnalysis analyze_editor(std::unique_ptr<clang::CompilerInstance> compiler,
                              clang::CompilerInstance& session, const std::string& code,
                              bool completion, bool signatures_only = false,
                              const std::set<std::string>& pch_headers = {},
                              const EditorPchCache::MacroSnapshot* macro_snapshot = nullptr,
                              const EditorPchCache::DeclSnapshot* decl_snapshot = nullptr);
const char* semantic_kind_name(SemanticKind kind);

} // namespace example
