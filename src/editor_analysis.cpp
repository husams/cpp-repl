#include "editor_analysis.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterSharedState.h"
#include "clang/AST/DeclLookups.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "cpp_lexer.hpp"
#include "cpp_symbols.hpp"
#include "definition_shadowing.hpp"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/MemoryBuffer.h"

namespace example {
namespace {

SemanticKind kind_of(const clang::NamedDecl* decl) {
  if (const auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    return kind_of(templ->getTemplatedDecl());
  }
  if (llvm::isa<clang::ParmVarDecl>(decl)) {
    return SemanticKind::Parameter;
  }
  if (llvm::isa<clang::FieldDecl>(decl)) {
    return SemanticKind::Field;
  }
  if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (variable->isStaticDataMember()) {
      return SemanticKind::Field;
    }
  }
  if (llvm::isa<clang::CXXMethodDecl>(decl)) {
    return SemanticKind::Method;
  }
  if (llvm::isa<clang::FunctionDecl, clang::FunctionTemplateDecl>(decl)) {
    return SemanticKind::Function;
  }
  if (llvm::isa<clang::TypeDecl, clang::ClassTemplateDecl, clang::TemplateTypeParmDecl>(decl)) {
    return SemanticKind::Type;
  }
  if (llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(decl)) {
    return SemanticKind::Namespace;
  }
  if (llvm::isa<clang::EnumConstantDecl>(decl)) {
    return SemanticKind::EnumConstant;
  }
  return SemanticKind::Variable;
}

CompletionItem describe(const clang::NamedDecl* decl) {
  CompletionItem item;
  const std::string qualified_name = decl->getQualifiedNameAsString();
  item.text = decl->getNameAsString();
  item.label = item.text;
  item.kind = kind_of(decl);
  if (const auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    decl = templ->getTemplatedDecl();
  }
  const auto& policy = decl->getASTContext().getPrintingPolicy();
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    item.label += '(';
    for (unsigned i = 0; i < function->getNumParams(); ++i) {
      if (i) {
        item.label += ", ";
      }
      const auto* param = function->getParamDecl(i);
      CompletionParameter parameter;
      parameter.name = param->getNameAsString();
      parameter.type = param->getType().getAsString(policy);
      parameter.has_default = param->hasDefaultArg();
      parameter.is_pack = param->isParameterPack();
      item.parameters.push_back(std::move(parameter));
      item.label += param->getType().getAsString(policy);
      if (!param->getName().empty()) {
        item.label += " " + param->getNameAsString();
      }
      if (param->hasDefaultArg()) {
        item.label += " = ...";
      }
    }
    if (function->isVariadic()) {
      item.variadic = true;
      if (function->getNumParams()) {
        item.label += ", ";
      }
      item.label += "...";
    }
    item.label += ')';
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
      if (method->isConst()) {
        item.label += " const";
      }
      if (method->isVolatile()) {
        item.label += " volatile";
      }
      if (method->getRefQualifier() == clang::RQ_LValue) {
        item.label += " &";
      }
      if (method->getRefQualifier() == clang::RQ_RValue) {
        item.label += " &&";
      }
    }
    item.detail = function->getReturnType().getAsString(policy);
  } else if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
    item.detail = value->getType().getAsString(policy);
  } else if (const auto* type = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
    item.detail = type->getUnderlyingType().getAsString(policy);
  }
  item.label = unversioned_name(std::move(item.label));
  item.detail = unversioned_name(std::move(item.detail));
  if (!item.parameters.empty() || item.kind == SemanticKind::Function ||
      item.kind == SemanticKind::Method) {
    item.identity = qualified_name + item.label.substr(item.text.size()) + " -> " + item.detail;
  }
  return item;
}

struct IncludePathPrefix {
  std::string logical;
  std::string replacement;
};

std::optional<IncludePathPrefix> included_path_prefix(llvm::StringRef code) {
  const auto lexed = lex_cpp(code.str());
  if (lexed.tokens.empty() || lexed.tokens.back().kind != TokenKind::Preprocessor ||
      lexed.tokens.back().end != code.size()) {
    return std::nullopt;
  }
  const auto raw = code.slice(lexed.tokens.back().begin, lexed.tokens.back().end);
  std::string logical;
  logical.reserve(raw.size());
  bool in_header = false;
  char header_end = '\0';
  std::size_t raw_path_begin = llvm::StringRef::npos;
  std::size_t raw_path_end = raw.size();
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\n') {
      i += 2;
    } else if (in_header) {
      if (raw[i] == header_end) {
        raw_path_end = i;
        break;
      }
      logical.push_back(raw[i++]);
    } else if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '*') {
      const auto close = raw.find("*/", i + 2);
      if (close == llvm::StringRef::npos) {
        return std::nullopt;
      }
      logical.push_back(' ');
      i = close + 2;
    } else if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
      // A continued physical line is still part of a // comment.
      i += 2;
      while (i < raw.size()) {
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\n') {
          i += 2;
        } else if (raw[i++] == '\n') {
          break;
        }
      }
    } else {
      if (raw[i] == '"' || raw[i] == '<') {
        in_header = true;
        header_end = raw[i] == '"' ? '"' : '>';
        raw_path_begin = i + 1;
      }
      logical.push_back(raw[i++]);
    }
  }
  llvm::StringRef rest(logical);
  const auto hash = rest.find('#');
  if (hash == llvm::StringRef::npos) {
    return std::nullopt;
  }
  rest = rest.drop_front(hash + 1).ltrim(" \t");
  const auto directive_end = rest.find_first_of(" \t<\"");
  const auto directive = rest.take_front(directive_end);
  if (directive != "include" && directive != "include_next" && directive != "import") {
    return std::nullopt;
  }
  if (directive_end == llvm::StringRef::npos) {
    return std::nullopt;
  }
  rest = rest.drop_front(directive_end).ltrim(" \t");
  if (rest.empty() || (rest.front() != '<' && rest.front() != '"')) {
    return std::nullopt;
  }
  if (raw_path_begin == llvm::StringRef::npos) {
    return std::nullopt;
  }
  return IncludePathPrefix{rest.drop_front().str(), raw.slice(raw_path_begin, raw_path_end).str()};
}

bool prefers_function_designator(clang::QualType type) {
  if (type.isNull()) {
    return false;
  }
  if (type->isReferenceType()) {
    type = type->getPointeeType();
  }
  if (type->isPointerType()) {
    type = type->getPointeeType();
  }
  return !type.isNull() && type->isFunctionType();
}

bool should_insert_call(const clang::CodeCompletionContext& context, llvm::StringRef before) {
  if (context.getKind() == clang::CodeCompletionContext::CCC_NewName ||
      context.isUsingDeclaration() || prefers_function_designator(context.getPreferredType())) {
    return false;
  }
  const auto tokens = lex_cpp(before.str()).tokens;
  bool in_using_declaration = false;
  auto it = tokens.rbegin();
  for (; it != tokens.rend(); ++it) {
    const auto token = before.substr(it->begin, it->end - it->begin);
    if (it->kind == TokenKind::Comment || (it->kind == TokenKind::Bracket && token == "(")) {
      continue;
    }
    break;
  }
  for (; it != tokens.rend(); ++it) {
    const auto token = before.substr(it->begin, it->end - it->begin);
    if (it->kind == TokenKind::Comment) {
      continue;
    }
    if (token == ";" || token == "{" || token == "}") {
      break;
    }
    if (token == "using") {
      in_using_declaration = true;
      break;
    }
  }
  auto address_it = tokens.rbegin();
  for (; address_it != tokens.rend(); ++address_it) {
    const auto token = before.substr(address_it->begin, address_it->end - address_it->begin);
    if (address_it->kind == TokenKind::Comment ||
        (address_it->kind == TokenKind::Bracket && token == "(")) {
      continue;
    }
    break;
  }
  bool address_operand = false;
  for (auto it = address_it; it != tokens.rend(); ++it) {
    const auto token = before.substr(it->begin, it->end - it->begin);
    if (it->kind == TokenKind::Comment || (it->kind == TokenKind::Bracket && token == "(")) {
      continue;
    }
    if (token == "&") {
      address_operand = true;
    }
    if (token != "::" && it->kind != TokenKind::Identifier && it->kind != TokenKind::Type &&
        token != "&") {
      break;
    }
    if (token == "&") {
      const auto previous = std::next(it);
      if (previous == tokens.rend()) {
        address_operand = true;
      } else {
        const auto prior_text = before.substr(previous->begin, previous->end - previous->begin);
        const bool prior_is_operand =
            previous->kind == TokenKind::Identifier || previous->kind == TokenKind::Type ||
            previous->kind == TokenKind::Number || previous->kind == TokenKind::String ||
            previous->kind == TokenKind::Char || prior_text == ")" || prior_text == "]" ||
            prior_text == "}";
        address_operand = !prior_is_operand;
      }
      break;
    }
  }
  return !in_using_declaration && !address_operand;
}

bool cursor_in_comment(llvm::StringRef code) {
  enum class State { Code, String, Character, LineComment, BlockComment };
  State state = State::Code;
  for (std::size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    const char next = i + 1 < code.size() ? code[i + 1] : '\0';
    switch (state) {
    case State::Code:
      if (c == '/' && next == '/') {
        state = State::LineComment;
        ++i;
      } else if (c == '/' && next == '*') {
        state = State::BlockComment;
        ++i;
      } else if (c == '"') {
        state = State::String;
      } else if (c == '\'') {
        state = State::Character;
      }
      break;
    case State::String:
    case State::Character:
      if (c == '\\' && i + 1 < code.size()) {
        ++i;
      } else if ((state == State::String && c == '"') ||
                 (state == State::Character && c == '\'')) {
        state = State::Code;
      }
      break;
    case State::LineComment:
      if (c == '\n') {
        state = State::Code;
      }
      break;
    case State::BlockComment:
      if (c == '*' && next == '/') {
        state = State::Code;
        ++i;
      }
      break;
    }
  }
  return state == State::LineComment || state == State::BlockComment;
}

class CompletionConsumer : public clang::CodeCompleteConsumer {
  clang::GlobalCodeCompletionAllocator allocator_;
  clang::CodeCompletionTUInfo info_{std::make_shared<clang::GlobalCodeCompletionAllocator>()};
  EditorAnalysis& result_;
  clang::CompilerInstance& session_;
  const std::string& code_;

public:
  // Without `load_external`, Clang enumerates only declarations already in the
  // editor AST instead of importing the whole session scope.
  CompletionConsumer(EditorAnalysis& result, clang::CompilerInstance& session,
                     const std::string& code, bool load_external)
      : CodeCompleteConsumer([&] {
        clang::CodeCompleteOptions options;
        options.IncludeGlobals = true;
        options.IncludeMacros = true;
        options.IncludeCodePatterns = true;
        options.IncludeBriefComments = true;
        options.LoadExternal = load_external;
        return options;
      }())
      , result_(result)
      , session_(session)
      , code_(code) {}

  clang::CodeCompletionAllocator& getAllocator() override { return allocator_; }

  clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() override { return info_; }

  void ProcessCodeCompleteResults(clang::Sema& sema, clang::CodeCompletionContext context,
                                  clang::CodeCompletionResult* results, unsigned count) override {
    const auto prefix = sema.getPreprocessor().getCodeCompletionFilter();
    const auto kind = context.getKind();
    const bool include_context = kind == clang::CodeCompletionContext::CCC_IncludedFile;
    const auto include_prefix = include_context ? included_path_prefix(code_) : std::nullopt;
    const auto include_logical_prefix = include_prefix ? include_prefix->logical : std::string();
    const auto include_directory =
        include_prefix ? include_logical_prefix.substr(
                             0, include_logical_prefix.find_last_of('/') == std::string::npos
                                    ? 0
                                    : include_logical_prefix.find_last_of('/') + 1)
                       : std::string();
    const auto include_name_prefix =
        include_prefix ? include_logical_prefix.substr(include_directory.size()) : std::string();
    if (include_prefix) {
      result_.completion_prefix = include_prefix->replacement;
    }
    const bool preprocessor_context =
        kind == clang::CodeCompletionContext::CCC_MacroName ||
        kind == clang::CodeCompletionContext::CCC_MacroNameUse ||
        kind == clang::CodeCompletionContext::CCC_PreprocessorExpression ||
        kind == clang::CodeCompletionContext::CCC_PreprocessorDirective ||
        kind == clang::CodeCompletionContext::CCC_IncludedFile;
    const bool macro_context = kind == clang::CodeCompletionContext::CCC_MacroName ||
                               kind == clang::CodeCompletionContext::CCC_MacroNameUse ||
                               kind == clang::CodeCompletionContext::CCC_PreprocessorExpression;
    if (cursor_in_comment(code_)) {
      return;
    }
    const auto before = llvm::StringRef(code_).drop_back(prefix.size()).rtrim();
    const bool insert_call = should_insert_call(context, before);
    for (unsigned i = 0; i < count; ++i) {
      auto& candidate = results[i];
      if (candidate.Hidden || candidate.Availability == CXAvailability_NotAvailable ||
          candidate.Availability == CXAvailability_NotAccessible) {
        continue;
      }
      if (preprocessor_context && candidate.Kind == clang::CodeCompletionResult::RK_Declaration) {
        continue;
      }
      CompletionItem item;
      if (candidate.Kind == clang::CodeCompletionResult::RK_Declaration) {
        const auto* decl = candidate.Declaration;
        if (!decl || !decl->getIdentifier() || decl->isInvalidDecl()) {
          continue;
        }
        item = describe(decl);
        item.insert_call = insert_call;
        if (auto* text =
                candidate.CreateCodeCompletionString(sema, context, allocator_, info_, true)) {
          if (const auto* comment = text->getBriefComment()) {
            item.documentation = comment;
          }
          item.priority = text->getPriority();
        }
      } else if (candidate.Kind == clang::CodeCompletionResult::RK_Keyword) {
        item.text = item.label = candidate.Keyword;
        item.kind = SemanticKind::Keyword;
        item.priority = candidate.Priority;
      } else if (candidate.Kind == clang::CodeCompletionResult::RK_Macro) {
        item.text = item.label = candidate.Macro->getName().str();
        item.kind = SemanticKind::Macro;
        item.priority = candidate.Priority;
      } else if (candidate.Kind == clang::CodeCompletionResult::RK_Pattern) {
        auto* completion =
            candidate.CreateCodeCompletionString(sema, context, allocator_, info_, true);
        if (!completion || !completion->getTypedText()) {
          continue;
        }
        // Statement keywords such as return arrive as code patterns too. Insert
        // only their typed text; placeholders belong to Clang's snippet model.
        item.text = item.label = completion->getTypedText();
        item.kind = SemanticKind::Keyword;
        item.insert_call = false;
        item.priority = completion->getPriority();
      } else {
        continue;
      }
      if (include_context) {
        item.is_header_path = true;
        if (!item.text.empty() && (item.text.back() == '"' || item.text.back() == '>')) {
          item.text.pop_back();
          item.label = item.text;
        }
        const llvm::StringRef candidate_path(item.text);
        const auto slash = candidate_path.rfind('/');
        const auto candidate_name = slash == llvm::StringRef::npos
                                        ? candidate_path
                                        : candidate_path.drop_front(slash + 1);
        if (!include_prefix || (!candidate_name.starts_with(include_name_prefix) &&
                                !candidate_path.starts_with(include_logical_prefix))) {
          continue;
        }
        // Clang's file completion may return either a basename or a relative
        // path. The editor replaces the whole typed path, so normalize each
        // candidate to that same shape before exposing it.
        if (!include_directory.empty() &&
            !llvm::StringRef(item.text).starts_with(include_directory)) {
          item.text = include_directory + item.text;
          item.label = item.text;
        }
      } else if (!llvm::StringRef(item.text).starts_with(prefix)) {
        continue;
      }
      if (!include_context && item.text.rfind("__", 0) == 0 &&
          !(macro_context && candidate.Kind == clang::CodeCompletionResult::RK_Macro)) {
        continue;
      }
      result_.items.push_back(std::move(item));
    }
    if (macro_context) {
      auto& preprocessor = sema.getPreprocessor();
      for (const auto& entry : preprocessor.macros()) {
        const auto* identifier = entry.first;
        const auto name = identifier->getName();
        if (!llvm::StringRef(name).starts_with(prefix) ||
            !preprocessor.getMacroDefinition(identifier).getMacroInfo()) {
          continue;
        }
        const auto duplicate =
            std::find_if(result_.items.begin(), result_.items.end(),
                         [&](const auto& existing) { return existing.text == name; });
        if (duplicate == result_.items.end()) {
          CompletionItem item;
          item.text = item.label = name.str();
          item.kind = SemanticKind::Macro;
          result_.items.push_back(std::move(item));
        }
      }
    }
    // Clang's ordinary-name enumeration can omit declarations relocated into
    // our version namespaces. Resolve their public names through Sema so local
    // shadowing still applies, and keep the full overload metadata.
    if (preprocessor_context) {
      return;
    }
    if (!context.getBaseType().isNull() || before.ends_with(".") || before.ends_with("->") ||
        before.ends_with("::")) {
      return;
    }
    const bool supplement_allowed =
        kind == clang::CodeCompletionContext::CCC_Statement ||
        kind == clang::CodeCompletionContext::CCC_Expression ||
        kind == clang::CodeCompletionContext::CCC_ParenthesizedExpression ||
        kind == clang::CodeCompletionContext::CCC_Symbol ||
        kind == clang::CodeCompletionContext::CCC_SymbolOrNewName ||
        kind == clang::CodeCompletionContext::CCC_TopLevelOrExpression ||
        (before.empty() &&
         (kind == clang::CodeCompletionContext::CCC_TopLevel ||
          kind == clang::CodeCompletionContext::CCC_Other ||
          kind == clang::CodeCompletionContext::CCC_OtherWithMacros ||
          kind == clang::CodeCompletionContext::CCC_Type));
    if (!supplement_allowed || !loadExternal()) {
      return;
    }
    for (const auto& qualified : user_symbol_names(session_.getASTContext())) {
      const auto separator = qualified.rfind("::");
      const auto name = qualified.substr(separator == std::string::npos ? 0 : separator + 2);
      // Sema decides whether namespace members are visible here, including
      // implicit anonymous-namespace and explicit using directives.
      if (name.rfind("__", 0) == 0 || !llvm::StringRef(name).starts_with(prefix)) {
        continue;
      }
      auto* id = &sema.getASTContext().Idents.get(name);
      clang::LookupResult lookup(sema, clang::DeclarationName(id), clang::SourceLocation(),
                                 clang::Sema::LookupOrdinaryName);
      lookup.suppressDiagnostics();
      if (!sema.LookupName(lookup, sema.getCurScope())) {
        continue;
      }
      for (const auto* decl : lookup) {
        if (!decl->getIdentifier() || decl->isInvalidDecl()) {
          continue;
        }
        if (decl->getAvailability() == clang::AR_Unavailable ||
            decl->getAvailability() == clang::AR_NotYetIntroduced) {
          continue;
        }
        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
          if (function->isDeleted()) {
            continue;
          }
        }
        auto item = describe(decl);
        item.insert_call = insert_call;
        const auto duplicate =
            std::find_if(result_.items.begin(), result_.items.end(), [&](const auto& existing) {
              return std::tie(existing.text, existing.label, existing.detail) ==
                     std::tie(item.text, item.label, item.detail);
            });
        if (duplicate == result_.items.end()) {
          result_.items.push_back(std::move(item));
        }
      }
    }
  }

  void ProcessOverloadCandidates(clang::Sema&, unsigned argument, OverloadCandidate* candidates,
                                 unsigned count, clang::SourceLocation, bool) override {
    result_.active_parameter = argument;
    for (unsigned i = 0; i < count; ++i) {
      if (const auto* function = candidates[i].getFunction()) {
        const auto item = describe(function);
        result_.signatures.push_back(item.label + " -> " + item.detail);
        const auto duplicate =
            std::find_if(result_.signature_items.begin(), result_.signature_items.end(),
                         [&](const auto& existing) { return existing.identity == item.identity; });
        if (duplicate == result_.signature_items.end()) {
          result_.signature_items.push_back(item);
        }
      }
    }
  }
};

// Function bodies withheld from the editor AST, with the session body to put
// back. A crash inside Clang skips destructors, so analyze_editor restores any
// entry still recorded here.
std::vector<std::tuple<clang::FunctionDecl*, clang::Stmt*, clang::SourceLocation>> withheld_bodies;

void restore_withheld_bodies(std::size_t size = 0) {
  while (withheld_bodies.size() > size) {
    auto [function, body, end] = withheld_bodies.back();
    withheld_bodies.pop_back();
    function->setBody(body);
    function->setRangeEnd(end);
  }
}

// Editor requests parse a few input tokens; they never need the bodies of
// ordinary session functions. Constant evaluation and return type deduction do.
bool withholds_body(const clang::FunctionDecl& function) {
  return function.doesThisDeclarationHaveABody() && !function.isConstexpr() &&
         !function.isDefaulted() && !function.getReturnType()->getContainedDeducedType();
}

// Import namespace shells lazily, but complete the declarations actually used.
// Importing every namespace member eagerly would resurrect obsolete versions
// and make a standard-library include expensive on every editor request.
class SessionImporter : public clang::ASTImporter {
  std::function<void(clang::Decl*, clang::Decl*)> imported_;

protected:
  llvm::Expected<clang::Decl*> ImportImpl(clang::Decl* decl) override {
    auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
    if (!space) {
      auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (!function || !withholds_body(*function)) {
        return ASTImporter::ImportImpl(decl);
      }
      // Import a declaration only; the session keeps its body.
      const auto depth = withheld_bodies.size();
      withheld_bodies.emplace_back(function, function->getBody(), function->getEndLoc());
      function->setBody(nullptr);
      auto imported = ASTImporter::ImportImpl(decl);
      restore_withheld_bodies(depth);
      return imported;
    }
    auto context = ImportContext(space->getDeclContext());
    if (!context) {
      return context.takeError();
    }
    auto begin = Import(space->getBeginLoc());
    if (!begin) {
      return begin.takeError();
    }
    auto location = Import(space->getLocation());
    if (!location) {
      return location.takeError();
    }
    auto* identifier =
        space->getIdentifier() ? &getToContext().Idents.get(space->getName()) : nullptr;
    clang::NamespaceDecl* target = nullptr;
    if (identifier) {
      for (auto* candidate : (*context)->noload_lookup(clang::DeclarationName(identifier))) {
        if ((target = llvm::dyn_cast<clang::NamespaceDecl>(candidate))) {
          break;
        }
      }
    } else if (auto* unit = llvm::dyn_cast<clang::TranslationUnitDecl>(*context)) {
      target = unit->getAnonymousNamespace();
    } else if (auto* parent = llvm::dyn_cast<clang::NamespaceDecl>(*context)) {
      target = parent->getAnonymousNamespace();
    }
    if (!target) {
      // Public names from version namespaces are published explicitly below.
      // Keep obsolete versions available for old objects without making their
      // names participate in ordinary or qualified lookup.
      const bool version = space->getName().starts_with("__cpp_repl_version_");
      target =
          clang::NamespaceDecl::Create(getToContext(), *context, space->isInline() && !version,
                                       *begin, *location, identifier, nullptr, space->isNested());
      (*context)->addDeclInternal(target);
      if (space->isAnonymousNamespace()) {
        if (auto* unit = llvm::dyn_cast<clang::TranslationUnitDecl>(*context)) {
          unit->setAnonymousNamespace(target);
        } else if (auto* parent = llvm::dyn_cast<clang::NamespaceDecl>(*context)) {
          parent->setAnonymousNamespace(target);
        }
      }
    }
    MapImported(space, target);
    return target;
  }

  void Imported(clang::Decl* from, clang::Decl* to) override { imported_(from, to); }

public:
  SessionImporter(clang::CompilerInstance& child, clang::CompilerInstance& parent,
                  std::function<void(clang::Decl*, clang::Decl*)> imported)
      // The default importer scans declarations for every name lookup. Build its
      // dedicated lookup index so importing a large namespace stays linear.
      : ASTImporter(child.getASTContext(), child.getFileManager(), parent.getASTContext(),
                    parent.getFileManager(), false,
                    std::make_shared<clang::ASTImporterSharedState>(
                        *child.getASTContext().getTranslationUnitDecl()))
      , imported_(std::move(imported)) {}
};

// Like clang-repl's completion importer, use a separate AST. Complete imports
// are needed for instantiated standard-library types (minimal imports conflict).
// Publish only current definitions and retain every overload. Version namespaces
// are implementation details; their current members also need ordinary lookup.
class SessionSource : public clang::ExternalASTSource {
  SessionImporter importer_;
  clang::CompilerInstance& parent_;
  clang::TranslationUnitDecl* child_;
  bool loaded_ = false;
  std::map<const clang::DeclContext*, std::vector<clang::NamespaceDecl*>> namespaces_;
  std::set<const clang::DeclContext*> loaded_namespaces_;
  std::set<std::pair<const clang::DeclContext*, std::string>> pending_lookups_;
  // Global names are imported lazily, one lookup name at a time.
  llvm::DenseSet<clang::DeclarationName> served_globals_;
  std::vector<clang::DeclarationName> pending_globals_;
  unsigned import_depth_ = 0;
  bool publishing_ = false;

  void copy_session_macros(clang::Preprocessor& target) {
    auto& source = parent_.getPreprocessor();
    auto& manager = parent_.getSourceManager();
    auto predefines = target.getPredefines();
    // Apply retained user directives after the fresh compiler's command-line
    // predefines. This also preserves #undef of a macro originally supplied by -D.
    for (const auto& entry : source.macros()) {
      const auto* id = entry.first;
      const auto* latest = source.getLocalMacroDirectiveHistory(id);
      if (!latest || !latest->getLocation().isValid()) {
        continue;
      }
      const auto origin = manager.getBufferName(latest->getLocation());
      if (origin == "<built-in>" || origin == "<command line>") {
        continue;
      }
      const auto* macro = source.getMacroDefinition(id).getMacroInfo();
      if (macro && macro->isBuiltinMacro()) {
        continue;
      }
      predefines += "\n#undef " + id->getName().str() + "\n";
      if (!macro) {
        continue;
      }
      predefines += "#define " + id->getName().str();
      if (macro->isFunctionLike()) {
        predefines += '(';
        for (unsigned i = 0; i < macro->getNumParams(); ++i) {
          if (i) {
            predefines += ',';
          }
          const bool last = i + 1 == macro->getNumParams();
          if (!(last && macro->isC99Varargs())) {
            predefines += macro->params()[i]->getName();
          }
          if (last && macro->isVariadic()) {
            predefines += "...";
          }
        }
        predefines += ')';
      }
      for (const auto& token : macro->tokens()) {
        predefines += ' ';
        predefines += clang::Lexer::getSpelling(token, manager, parent_.getLangOpts());
      }
      predefines += '\n';
    }
    target.setPredefines(predefines);
  }

  void define_type(clang::QualType type) {
    type = type.getNonReferenceType();
    while (!type.isNull() && type->isPointerType()) {
      type = type->getPointeeType();
    }
    if (!type.isNull()) {
      if (auto* record = type->getAsCXXRecordDecl()) {
        if (record->hasDefinition()) {
          if (auto error = importer_.ImportDefinition(record->getDefinition())) {
            llvm::consumeError(std::move(error));
          }
        }
      }
    }
  }

  // Lookup context of a session declaration, looking through version namespaces.
  static const clang::DeclContext* lookup_context(const clang::Decl& decl) {
    const auto* context = decl.getDeclContext();
    if (const auto* space = llvm::dyn_cast_or_null<clang::NamespaceDecl>(context)) {
      if (space->getName().starts_with("__cpp_repl_version_")) {
        context = context->getParent();
      }
    }
    return context ? context->getRedeclContext() : nullptr;
  }

  void imported(clang::Decl* from, clang::Decl* to) {
    if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(to)) {
      auto* primary = space->getPrimaryContext();
      auto& sources = namespaces_[primary];
      // A namespace lookup already spans all of its redeclarations. Retaining
      // every reopening here repeats the same lookup and, during completion,
      // traverses the entire redeclaration chain once per reopening.
      auto* original = llvm::cast<clang::NamespaceDecl>(from)->getCanonicalDecl();
      if (std::find(sources.begin(), sources.end(), original) == sources.end()) {
        sources.push_back(original);
      }
      primary->setHasExternalVisibleStorage(true);
    }
    // A dependency creates the editor's lookup entry for its name, and Clang
    // then stops asking this source for that name. Import the remaining
    // declarations of the name too, so every overload stays visible.
    const auto* named = llvm::dyn_cast<clang::NamedDecl>(to);
    const auto* context = lookup_context(*from);
    if (named && !named->getDeclName().isEmpty() && context && context->isTranslationUnit() &&
        !served_globals_.contains(named->getDeclName())) {
      pending_globals_.push_back(named->getDeclName());
    }
  }

  // Maps an editor lookup name to the session AST.
  std::optional<clang::DeclarationName> session_name(clang::DeclarationName name) {
    auto& context = parent_.getASTContext();
    switch (name.getNameKind()) {
    case clang::DeclarationName::Identifier:
      if (const auto* identifier = name.getAsIdentifierInfo()) {
        return clang::DeclarationName(&context.Idents.get(identifier->getName()));
      }
      return std::nullopt;
    case clang::DeclarationName::CXXOperatorName:
      return context.DeclarationNames.getCXXOperatorName(name.getCXXOverloadedOperator());
    case clang::DeclarationName::CXXLiteralOperatorName:
      return context.DeclarationNames.getCXXLiteralOperatorName(
          &context.Idents.get(name.getCXXLiteralIdentifier()->getName()));
    case clang::DeclarationName::CXXUsingDirective:
      return clang::DeclarationName::getUsingDirectiveName();
    case clang::DeclarationName::CXXDeductionGuideName:
      if (const auto from = importer_.getImportedFromDecl(name.getCXXDeductionGuideTemplate())) {
        return context.DeclarationNames.getCXXDeductionGuideName(*from);
      }
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }

  // Imports the current session declarations of one global-scope name.
  bool import_global(clang::DeclarationName name) {
    if (!served_globals_.insert(name).second) {
      return false;
    }
    const auto parent_name = session_name(name);
    if (!parent_name) {
      return false;
    }
    bool any = false;
    for (auto* decl : parent_.getASTContext().getTranslationUnitDecl()->lookup(*parent_name)) {
      import(decl);
      any = true;
    }
    return any;
  }

  void publish_imported_globals() {
    if (publishing_) {
      return;
    }
    publishing_ = true;
    while (!pending_globals_.empty()) {
      const auto name = pending_globals_.back();
      pending_globals_.pop_back();
      import_global(name);
    }
    publishing_ = false;
  }

  void import(clang::Decl* decl) {
    ++import_depth_;
    import_declaration(decl);
    if (--import_depth_ == 0) {
      publish_imported_globals();
    }
  }

  void import_declaration(clang::Decl* decl) {
    auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
    if (!named || named->isInvalidDecl()) {
      return;
    }
    if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(named)) {
      if (space->getName().starts_with("__cpp_repl_version_")) {
        for (auto* member : space->decls()) {
          import(member);
        }
        return;
      }
    }
    if (!llvm::isa<clang::UsingDirectiveDecl>(named) && !is_current_definition(*named)) {
      return;
    }
    auto imported = importer_.Import(named);
    if (!imported) {
      llvm::consumeError(imported.takeError());
      return;
    }
    if (auto* visible = llvm::dyn_cast<clang::NamedDecl>(*imported)) {
      auto* context = visible->getDeclContext();
      if (const auto* space = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
        if (space->getName().starts_with("__cpp_repl_version_")) {
          context = context->getParent();
        }
      }
      context->makeDeclVisibleInContext(visible);
    }
    if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(named)) {
      define_type(value->getType());
    }
    if (auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(named)) {
      if (record->hasDefinition()) {
        if (auto error = importer_.ImportDefinition(record->getDefinition())) {
          llvm::consumeError(std::move(error));
        }
      }
    }
  }

public:
  SessionSource(clang::CompilerInstance& child, clang::CompilerInstance& parent)
      : importer_(child, parent, [this](clang::Decl* from, clang::Decl* to) { imported(from, to); })
      , parent_(parent)
      , child_(child.getASTContext().getTranslationUnitDecl()) {
    copy_session_macros(child.getPreprocessor());
  }

  bool FindExternalVisibleDeclsByName(const clang::DeclContext* context,
                                      clang::DeclarationName name,
                                      const clang::DeclContext*) override {
    if (context->isTranslationUnit()) {
      return import_global(name);
    }
    const auto found = namespaces_.find(context->getPrimaryContext());
    if (found == namespaces_.end() || !name.isIdentifier()) {
      return false;
    }
    // Publishing an imported declaration can ask this source for that same
    // name again (notably std::cout's inline namespace). Break that cycle while
    // the outer request fills the lookup entry.
    const auto key = std::make_pair(context->getPrimaryContext(), name.getAsString());
    if (!pending_lookups_.insert(key).second) {
      return false;
    }
#if LLVM_VERSION_MAJOR >= 22
    const llvm::scope_exit cleanup([&] { pending_lookups_.erase(key); });
#else
    const auto cleanup = llvm::make_scope_exit([&] { pending_lookups_.erase(key); });
#endif
    const auto parent_name =
        clang::DeclarationName(&parent_.getASTContext().Idents.get(name.getAsString()));
    bool any = false;
    const auto sources = found->second;
    for (auto* space : sources) {
      for (auto* decl : space->lookup(parent_name)) {
        import(decl);
        any = true;
      }
    }
    return any;
  }

  void completeVisibleDeclsMap(const clang::DeclContext* context) override {
    if (!context->isTranslationUnit()) {
      auto* primary = context->getPrimaryContext();
      if (!loaded_namespaces_.insert(primary).second) {
        return;
      }
      const auto found = namespaces_.find(primary);
      if (found == namespaces_.end()) {
        return;
      }
      const auto sources = found->second;
      for (auto* space : sources) {
        for (auto* previous : space->redecls()) {
          for (auto* decl : previous->decls()) {
            import(decl);
          }
        }
      }
      primary->setHasExternalVisibleStorage(false);
      return;
    }
    if (loaded_) {
      return;
    }
    loaded_ = true;
    for (auto* unit = parent_.getASTContext().getTranslationUnitDecl(); unit;
         unit = unit->getPreviousDecl()) {
      for (auto* decl : unit->decls()) {
        import(decl);
      }
    }
    child_->setHasExternalVisibleStorage(false);
  }
};

class TokenVisitor : public clang::RecursiveASTVisitor<TokenVisitor> {
  clang::ASTContext& context_;
  EditorAnalysis& result_;
  std::size_t size_;

  void add(clang::SourceLocation location, SemanticKind kind) {
    auto& manager = context_.getSourceManager();
    if (!location.isValid() || location.isMacroID() || !manager.isWrittenInMainFile(location)) {
      return;
    }
    const auto begin = manager.getFileOffset(location);
    const auto length = clang::Lexer::MeasureTokenLength(location, manager, context_.getLangOpts());
    if (begin < size_ && length && begin + length <= size_) {
      result_.tokens.push_back({begin, begin + length, kind});
    }
  }

public:
  TokenVisitor(clang::ASTContext& context, EditorAnalysis& result, std::size_t size)
      : context_(context)
      , result_(result)
      , size_(size) {}

  bool TraverseDecl(clang::Decl* decl) {
    if (decl && !llvm::isa<clang::TranslationUnitDecl>(decl) && decl->getLocation().isValid() &&
        !context_.getSourceManager().isWrittenInMainFile(decl->getLocation())) {
      return true;
    }
    return RecursiveASTVisitor::TraverseDecl(decl);
  }

  bool VisitNamedDecl(clang::NamedDecl* decl) {
    if (!decl->isImplicit() && decl->getIdentifier()) {
      add(decl->getLocation(), kind_of(decl));
    }
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
    add(expr->getLocation(), kind_of(expr->getDecl()));
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr* expr) {
    add(expr->getMemberLoc(), kind_of(expr->getMemberDecl()));
    return true;
  }

  bool VisitTypeLoc(clang::TypeLoc loc) {
    if (loc.getType()->isBuiltinType()) {
      return true;
    }
    if (loc.getAs<clang::TypedefTypeLoc>() || loc.getAs<clang::TagTypeLoc>() ||
        loc.getAs<clang::TemplateSpecializationTypeLoc>() ||
        loc.getAs<clang::TemplateTypeParmTypeLoc>()) {
      add(loc.getBeginLoc(), SemanticKind::Type);
    }
    return true;
  }

  bool VisitNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc loc) {
#if LLVM_VERSION_MAJOR >= 22
    if (loc.getNestedNameSpecifier().getKind() == clang::NestedNameSpecifier::Kind::Namespace)
#else
    if (loc.getNestedNameSpecifier()->getKind() == clang::NestedNameSpecifier::Namespace ||
        loc.getNestedNameSpecifier()->getKind() == clang::NestedNameSpecifier::NamespaceAlias)
#endif
      add(loc.getLocalBeginLoc(), SemanticKind::Namespace);
    return true;
  }
};

class TokenConsumer : public clang::ASTConsumer {
  EditorAnalysis& result_;
  std::size_t size_;

public:
  TokenConsumer(EditorAnalysis& result, std::size_t size)
      : result_(result)
      , size_(size) {}

  void HandleTranslationUnit(clang::ASTContext& context) override {
    TokenVisitor visitor(context, result_, size_);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }
};

class MacroExpansionColors : public clang::PPCallbacks {
  clang::SourceManager& source_;
  const clang::LangOptions& language_;
  EditorAnalysis& result_;
  std::size_t size_;

public:
  MacroExpansionColors(clang::SourceManager& source, const clang::LangOptions& language,
                       EditorAnalysis& result, std::size_t size)
      : source_(source)
      , language_(language)
      , result_(result)
      , size_(size) {}

  void MacroExpands(const clang::Token& token, const clang::MacroDefinition&, clang::SourceRange,
                    const clang::MacroArgs*) override {
    if (!token.getIdentifierInfo()) {
      return;
    }
    const auto location = source_.getFileLoc(token.getLocation());
    if (!location.isValid() || !source_.isWrittenInMainFile(location)) {
      return;
    }
    const auto begin = source_.getFileOffset(location);
    const auto length = clang::Lexer::MeasureTokenLength(location, source_, language_);
    if (length && begin + length <= size_) {
      result_.tokens.push_back({begin, begin + length, SemanticKind::Macro});
    }
  }
};

class EditorAction : public clang::SyntaxOnlyAction {
  clang::CompilerInstance& parent_;
  EditorAnalysis& result_;
  std::size_t size_;

public:
  EditorAction(clang::CompilerInstance& parent, EditorAnalysis& result, std::size_t size)
      : parent_(parent)
      , result_(result)
      , size_(size) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
                                                        llvm::StringRef) override {
    return std::make_unique<TokenConsumer>(result_, size_);
  }

  void ExecuteAction() override {
    auto& child = getCompilerInstance();
    auto source = llvm::makeIntrusiveRefCnt<SessionSource>(child, parent_);
    child.getPreprocessor().addPPCallbacks(std::make_unique<MacroExpansionColors>(
        child.getSourceManager(), child.getLangOpts(), result_, size_));
    child.getASTContext().setExternalSource(source);
    // Global names are imported on lookup. Enumerating the global scope, as
    // unqualified completion does, still imports all of it.
    child.getASTContext().getTranslationUnitDecl()->setHasExternalVisibleStorage(true);
    // Keep REPL top-level statements, but terminate this finite buffer with EOF.
    // LLVM 21/22's lexer emits annot_repl_input_end whenever the language's REPL
    // extensions are enabled, independently of IncrementalProcessing. Parser
    // recovery inside unfinished bodies or initializers can loop at this token.
    // Preprocessor::Lex passes its mutable result token to this observer through
    // a const reference; normalize only that token in our disposable compiler.
    child.getPreprocessor().setTokenWatcher([](const clang::Token& token) {
      if (token.is(clang::tok::annot_repl_input_end)) {
        const_cast<clang::Token&>(token).setKind(clang::tok::eof);
      }
    });
    child.getPreprocessor().enableIncrementalProcessing(false);
    SyntaxOnlyAction::ExecuteAction();
  }
};

// ASTImporter can diagnose the original AST. Keep diagnostics and error state
// local to this request, including after a previously rejected session input.
class DiagnosticScope {
  clang::DiagnosticsEngine& diagnostics_;
  bool had_errors_;
  std::unique_ptr<clang::DiagnosticConsumer> owned_;
  clang::DiagnosticConsumer* previous_;

public:
  explicit DiagnosticScope(clang::DiagnosticsEngine& diagnostics)
      : diagnostics_(diagnostics)
      , had_errors_(diagnostics.hasErrorOccurred())
      , owned_(diagnostics.takeClient())
      , previous_(diagnostics.getClient()) {
    diagnostics_.setClient(std::make_unique<clang::IgnoringDiagConsumer>().release(), true);
  }

  ~DiagnosticScope() {
    if (owned_) {
      diagnostics_.setClient(owned_.release(), true);
    } else {
      diagnostics_.setClient(previous_, false);
    }
    if (!had_errors_) {
      diagnostics_.Reset(true);
    }
  }
};

} // namespace

const char* semantic_kind_name(SemanticKind kind) {
  switch (kind) {
  case SemanticKind::Variable:
    return "variable";
  case SemanticKind::Parameter:
    return "parameter";
  case SemanticKind::Field:
    return "field";
  case SemanticKind::Function:
    return "function";
  case SemanticKind::Method:
    return "method";
  case SemanticKind::Type:
    return "type";
  case SemanticKind::Namespace:
    return "namespace";
  case SemanticKind::EnumConstant:
    return "enum";
  case SemanticKind::Macro:
    return "macro";
  case SemanticKind::Keyword:
    return "keyword";
  }
  return "";
}

EditorAnalysis analyze_editor(std::unique_ptr<clang::CompilerInstance> compiler,
                              clang::CompilerInstance& session, const std::string& code,
                              bool completion, bool signatures_only) {
  EditorAnalysis result;
  if (code.empty() || code.size() > 65536) {
    return result;
  }
  DiagnosticScope diagnostics(session.getDiagnostics());
  compiler->getDiagnostics().setClient(std::make_unique<clang::IgnoringDiagConsumer>().release(),
                                       true);
  // Diagnostics are discarded, and typo correction would look up, and so
  // import, every similar session name.
  compiler->getLangOpts().SpellChecking = false;
  const std::string filename = "input_line_[Editor]";
  auto buffer = llvm::MemoryBuffer::getMemBufferCopy(code + "\n;", filename);
  compiler->getPreprocessorOpts().addRemappedFile(filename, buffer.release());
  compiler->getFrontendOpts().Inputs = {clang::FrontendInputFile(filename, clang::Language::CXX)};
  if (completion) {
    const auto last_newline = code.rfind('\n');
    const unsigned line = std::count(code.begin(), code.end(), '\n') + 1;
    const unsigned column =
        code.size() - (last_newline == std::string::npos ? 0 : last_newline + 1) + 1;
    compiler->getFrontendOpts().CodeCompletionAt = {filename, line, column};
    compiler->setCodeCompletionConsumer(
        std::make_unique<CompletionConsumer>(result, session, code, !signatures_only).release());
  }
  auto action = std::make_unique<EditorAction>(session, result, code.size());
  llvm::CrashRecoveryContext::Enable();
  llvm::CrashRecoveryContext recovery;
  const bool completed = recovery.RunSafely([&] { compiler->ExecuteAction(*action); });
  llvm::CrashRecoveryContext::Disable();
  if (!completed) {
    // The session must get back any function body withheld during the crash.
    restore_withheld_bodies();
    // Clang may be partially torn down after a native crash. Avoid destructing
    // that compiler; the persistent interpreter has not parsed this buffer.
    (void)action.release();
    (void)compiler.release();
    return {};
  }
  std::sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
    return std::tie(a.priority, a.text, a.label, a.detail) <
           std::tie(b.priority, b.text, b.label, b.detail);
  });
  std::set<std::tuple<std::string, std::string, std::string>> seen;
  result.items.erase(
      std::remove_if(result.items.begin(), result.items.end(),
                     [&](const auto& item) {
                       return !seen.emplace(item.text, item.label, item.detail).second;
                     }),
      result.items.end());
  std::sort(result.signatures.begin(), result.signatures.end());
  result.signatures.erase(std::unique(result.signatures.begin(), result.signatures.end()),
                          result.signatures.end());
  std::sort(result.signature_items.begin(), result.signature_items.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.identity < rhs.identity; });
  return result;
}
} // namespace example
