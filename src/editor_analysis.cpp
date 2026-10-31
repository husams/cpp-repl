#include "editor_analysis.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImportError.h"
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
#include "clang/Sema/MultiplexExternalSemaSource.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "cpp_lexer.hpp"
#include "cpp_symbols.hpp"
#include "definition_shadowing.hpp"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/ADT/SmallString.h"

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
  bool signatures_only_;

public:
  CompletionConsumer(EditorAnalysis& result, clang::CompilerInstance& session,
                     const std::string& code, bool signatures_only)
      : CodeCompleteConsumer([signatures_only] {
        clang::CodeCompleteOptions options;
        options.IncludeGlobals = true;
        // Clang's default macro enumeration deserializes every macro from the
        // PCH before applying the completion prefix. We add matching macros
        // from the live/session and edited-buffer preprocessor state below.
        options.IncludeMacros = false;
        options.IncludeCodePatterns = true;
        options.IncludeBriefComments = true;
        options.LoadExternal = !signatures_only;
        return options;
      }())
      , result_(result)
      , session_(session)
      , code_(code)
      , signatures_only_(signatures_only) {}

  clang::CodeCompletionAllocator& getAllocator() override { return allocator_; }

  clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() override { return info_; }

  void ProcessCodeCompleteResults(clang::Sema& sema, clang::CodeCompletionContext context,
                                  clang::CodeCompletionResult* results, unsigned count) override {
    if (signatures_only_) {
      return;
    }
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
    const bool ordinary_macro_context =
        !preprocessor_context && context.getBaseType().isNull() &&
        !context.getCXXScopeSpecifier() &&
        (kind == clang::CodeCompletionContext::CCC_TopLevel ||
         kind == clang::CodeCompletionContext::CCC_ClassStructUnion ||
         kind == clang::CodeCompletionContext::CCC_Other ||
         kind == clang::CodeCompletionContext::CCC_OtherWithMacros ||
         kind == clang::CodeCompletionContext::CCC_Statement ||
         kind == clang::CodeCompletionContext::CCC_Expression ||
         kind == clang::CodeCompletionContext::CCC_ParenthesizedExpression ||
         kind == clang::CodeCompletionContext::CCC_Type ||
         kind == clang::CodeCompletionContext::CCC_Recovery ||
         kind == clang::CodeCompletionContext::CCC_TopLevelOrExpression);
    if (macro_context || ordinary_macro_context) {
      auto& preprocessor = sema.getPreprocessor();
      // Gather names before resolving any external/PCH identifiers: resolving
      // one can update the preprocessor's macro table and invalidate iterators.
      std::set<std::string> macro_names;
      for (const auto& entry : session_.getPreprocessor().macros(false)) {
        macro_names.insert(entry.first->getName().str());
      }
      for (const auto& entry : preprocessor.macros(false)) {
        macro_names.insert(entry.first->getName().str());
      }
      const bool include_undefined =
          kind == clang::CodeCompletionContext::CCC_PreprocessorExpression;
      const auto preferred = context.getPreferredType();
      const bool preferred_pointer =
          !preferred.isNull() &&
          (preferred->isAnyPointerType() || preferred->isMemberPointerType() ||
           preferred->isBlockPointerType());
      for (const auto& name : macro_names) {
        if (!llvm::StringRef(name).starts_with(prefix) ||
            (!macro_context && name.rfind("__", 0) == 0)) {
          continue;
        }
        auto* identifier = &sema.getASTContext().Idents.get(name);
        if (identifier->isOutOfDate()) {
          if (auto* external = preprocessor.getExternalSource()) {
            external->updateOutOfDateIdentifier(*identifier);
          }
        }
        const auto definition = preprocessor.getMacroDefinition(identifier);
        const auto* macro = definition.getMacroInfo();
        if ((!macro && !include_undefined) ||
            (macro && macro->isUsedForHeaderGuard() && !macro_context)) {
          continue;
        }
        const auto duplicate =
            std::find_if(result_.items.begin(), result_.items.end(),
                         [&](const auto& existing) { return existing.text == name; });
        if (duplicate == result_.items.end()) {
          CompletionItem item;
          item.text = item.label = name;
          item.kind = SemanticKind::Macro;
          item.priority = clang::getMacroUsagePriority(name, sema.getLangOpts(),
                                                       preferred_pointer);
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
    if (!supplement_allowed) {
      return;
    }
    if (signatures_only_) {
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

// Import namespace shells lazily, but complete the declarations actually used.
// Importing every namespace member eagerly would resurrect obsolete versions
// and make a standard-library include expensive on every editor request.
class SessionImporter : public clang::ASTImporter {
  const std::set<std::string>& pch_headers_;
  clang::ASTReader* pch_reader_;
  std::map<std::pair<const clang::SourceManager*, unsigned>, std::string> canonical_paths_;
  std::set<std::pair<const clang::DeclContext*, clang::DeclarationName>> pending_namespace_lookups_;
  std::set<clang::DeclarationName> imported_global_names_;

  static bool is_specialization(const clang::Decl* decl) {
    if (llvm::isa<clang::ClassTemplateSpecializationDecl,
                  clang::VarTemplateSpecializationDecl>(decl)) {
      return true;
    }
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      return function->getTemplateSpecializationKind() != clang::TSK_Undeclared;
    }
    return false;
  }

  std::optional<std::pair<std::string, unsigned>> file_location(
      const clang::SourceManager& source, clang::SourceLocation location) {
    if (location.isInvalid() || !location.isFileID()) {
      return std::nullopt;
    }
    const auto key = std::make_pair(&source, source.getFileID(location).getHashValue());
    auto found = canonical_paths_.find(key);
    if (found == canonical_paths_.end()) {
      llvm::SmallString<256> canonical;
      const auto filename = source.getFilename(location);
      if (filename.empty() || llvm::sys::fs::real_path(filename, canonical)) {
        return std::nullopt;
      }
      found = canonical_paths_.emplace(key, canonical.str().str()).first;
    }
    return std::make_pair(found->second, source.getFileOffset(location));
  }

  std::optional<std::pair<std::string, std::string>> canonical_spelling_location(
      const clang::Decl* decl) {
    const auto& source = decl->getASTContext().getSourceManager();
    auto location = decl->getLocation();
    std::string header;
    std::string identity;
    // Include every expansion site: repeated (and nested) macro invocations
    // share their spelling token but can declare different overloads/types.
    for (;;) {
      const auto anchor = file_location(source, source.getSpellingLoc(location));
      if (!anchor) {
        return std::nullopt;
      }
      if (header.empty()) {
        header = anchor->first;
      }
      identity += anchor->first;
      identity.push_back('\0');
      identity += std::to_string(anchor->second);
      identity.push_back('\0');
      if (!location.isMacroID()) {
        break;
      }
      location = source.getImmediateMacroCallerLoc(location);
    }
    return std::make_pair(std::move(header), std::move(identity));
  }

  static clang::NamedDecl* template_pattern(clang::NamedDecl* decl) {
    if (auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      return templ->getTemplatedDecl();
    }
    if (auto* templ = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
      return templ->getTemplatedDecl();
    }
    if (auto* templ = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
      return templ->getTemplatedDecl();
    }
    if (auto* templ = llvm::dyn_cast<clang::VarTemplateDecl>(decl)) {
      return templ->getTemplatedDecl();
    }
    return decl;
  }

  clang::NamedDecl* find_pch_equivalent(clang::NamedDecl* source) {
    if (pch_headers_.empty() || !source || source->getDeclName().isEmpty()) {
      return nullptr;
    }
    if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(source)) {
      if (function->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization ||
          function->getPrimaryTemplate() || source->getDeclContext()->isFunctionOrMethod()) {
        return nullptr;
      }
      auto* pattern = function->getInstantiatedFromMemberFunction();
      if (pattern) {
        const auto pattern_location = canonical_spelling_location(pattern);
        if (!pattern_location || !pch_headers_.count(pattern_location->first)) {
          return nullptr;
        }
        auto* imported_owner = GetAlreadyImportedOrNull(
            clang::Decl::castFromDeclContext(source->getDeclContext()));
        auto* target_context = imported_owner
                                  ? clang::Decl::castToDeclContext(imported_owner)
                                  : nullptr;
        if (!target_context) {
          return nullptr;
        }
        auto* target_specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(imported_owner);
        if (!target_specialization ||
            !target_specialization->getSpecializedTemplate()->isFromASTFile()) {
          return nullptr;
        }
        for (auto* candidate : target_context->decls()) {
          auto* target_function = llvm::dyn_cast<clang::FunctionDecl>(candidate);
          if (!target_function || target_function->getKind() != function->getKind()) {
            continue;
          }
          auto* target_pattern = target_function->getInstantiatedFromMemberFunction();
          const auto target_location = target_pattern
                                           ? canonical_spelling_location(target_pattern)
                                           : std::nullopt;
          if (target_pattern && target_location &&
              *target_location == *pattern_location) {
            return target_function;
          }
        }
        return nullptr;
      }
    }
    if (is_specialization(source)) {
      return nullptr;
    }
    if (source->getDeclContext()->isFunctionOrMethod()) {
      return nullptr;
    }
    const auto source_location = canonical_spelling_location(source);
    if (!source_location || !pch_headers_.count(source_location->first)) {
      return nullptr;
    }
    clang::DeclContext* target_context = nullptr;
    auto* owner = source->getDeclContext();
    if (owner->isFileContext()) {
      auto imported_context = ImportContext(owner);
      if (!imported_context) {
        llvm::consumeError(imported_context.takeError());
        return nullptr;
      }
      target_context = *imported_context;
    } else if (auto* imported_owner =
                   GetAlreadyImportedOrNull(clang::Decl::castFromDeclContext(owner))) {
      target_context = clang::Decl::castToDeclContext(imported_owner);
    } else {
      // Parameters and template arguments can be imported before their owner
      // is mapped. Importing that owner here would recursively import this decl.
      return nullptr;
    }
    clang::DeclarationName target_name = name_in(source->getDeclName(), getToContext());
    if (target_name.isEmpty()) {
      return nullptr;
    }
    auto candidates = target_context->noload_lookup(target_name);
    // An imported forward declaration can make lookup nonempty while the
    // matching PCH redeclaration is still external. Ask the reader for this
    // one name even in that case, with a per-context/name reentrancy guard.
    const auto key = std::make_pair(static_cast<const clang::DeclContext*>(target_context),
                                    target_name);
    auto* primary_context = target_context->getPrimaryContext();
    if (pch_reader_ && primary_context->hasExternalVisibleStorage() &&
        pending_namespace_lookups_.insert(key).second) {
      pch_reader_->FindExternalVisibleDeclsByName(primary_context, target_name, target_context);
      candidates = target_context->noload_lookup(target_name);
      pending_namespace_lookups_.erase(key);
    }
    for (auto* candidate : candidates) {
      auto* named = llvm::dyn_cast<clang::NamedDecl>(candidate);
      if (!named || !named->isFromASTFile() || is_specialization(named)) {
        continue;
      }
      // Lookup can return friend-injected declarations whose lexical owner is
      // a record even though their semantic context is this namespace. Only
      // record members that ASTImporter may reorder need exact lexical-owner
      // membership here.
      if ((llvm::isa<clang::FieldDecl>(named) ||
           llvm::isa<clang::IndirectFieldDecl>(named)) &&
          (named->getLexicalDeclContext() != target_context ||
           !target_context->containsDeclAndLoad(named))) {
        continue;
      }
      for (auto* pattern : {named, template_pattern(named)}) {
        if (!pattern || is_specialization(pattern) || pattern->getKind() != source->getKind()) {
          continue;
        }
        const auto target_location = canonical_spelling_location(pattern);
        if (target_location && *target_location == *source_location) {
          return pattern;
        }
      }
      if (auto* templ = llvm::dyn_cast<clang::ClassTemplateDecl>(named)) {
        for (auto* redeclaration : templ->redecls()) {
          if (!redeclaration->isFromASTFile()) {
            continue;
          }
          auto* templated_record = redeclaration->getTemplatedDecl();
          if (source->getKind() == redeclaration->getKind()) {
            const auto redeclaration_location = canonical_spelling_location(redeclaration);
            if (redeclaration_location && *redeclaration_location == *source_location) {
              return redeclaration;
            }
          }
          if (source->getKind() == templated_record->getKind()) {
            const auto record_location = canonical_spelling_location(templated_record);
            if (templated_record->isFromASTFile() && record_location &&
                *record_location == *source_location) {
              return templated_record;
            }
          }
        }
        if (auto* definition = templ->getTemplatedDecl()->getDefinition()) {
          const auto definition_location = canonical_spelling_location(definition);
          if (definition_location && *definition_location == *source_location) {
            if (source->getKind() == templ->getKind()) {
              auto* definition_template = definition->getDescribedClassTemplate();
              if (definition_template && definition_template->isFromASTFile()) {
                return definition_template;
              }
            }
            if (source->getKind() == definition->getKind() && definition->isFromASTFile()) {
              return definition;
            }
          }
        }
      }
    }
    return nullptr;
  }

  clang::ClassTemplateSpecializationDecl* import_pch_implicit_specialization(
      clang::ClassTemplateSpecializationDecl* source) {
    if (!source || source->getSpecializationKind() != clang::TSK_ImplicitInstantiation) {
      return nullptr;
    }
    const auto instantiated_from = source->getInstantiatedFrom();
    clang::NamedDecl* selected_pattern = nullptr;
    if (llvm::isa<clang::ClassTemplateDecl*>(instantiated_from)) {
      selected_pattern = llvm::cast<clang::ClassTemplateDecl*>(instantiated_from);
    } else if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl*>(instantiated_from)) {
      selected_pattern =
          llvm::cast<clang::ClassTemplatePartialSpecializationDecl*>(instantiated_from);
    }
    const auto pattern_location =
        selected_pattern ? canonical_spelling_location(selected_pattern) : std::nullopt;
    if (!pattern_location || !pch_headers_.count(pattern_location->first)) {
      return nullptr;
    }
    auto* source_template = source->getSpecializedTemplate();
    if (!find_pch_equivalent(source_template)) {
      return nullptr;
    }
    auto imported_template = Import(source_template);
    if (!imported_template) {
      llvm::consumeError(imported_template.takeError());
      return nullptr;
    }
    auto* target_template = llvm::dyn_cast<clang::ClassTemplateDecl>(*imported_template);
    if (!target_template) {
      return nullptr;
    }
    std::vector<clang::TemplateArgument> arguments;
    for (const auto& argument : source->getTemplateArgs().asArray()) {
      if (argument.getKind() != clang::TemplateArgument::Type) {
        return nullptr;
      }
      auto imported_type = Import(argument.getAsType());
      if (!imported_type) {
        llvm::consumeError(imported_type.takeError());
        return nullptr;
      }
      arguments.emplace_back(getToContext().getCanonicalType(*imported_type));
    }
    void* insert_position = nullptr;
    auto* target = target_template->findSpecialization(arguments, insert_position);
    if (target) {
      auto* definition = llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          target->getDefinition());
      if (!definition || !definition->isCompleteDefinition()) {
        return nullptr;
      }
      // findSpecialization can return a forward redeclaration while the PCH
      // definition lives in a later redeclaration. Map to the definition so
      // member lookup and ASTImporter see the definition's lexical context.
      target = definition;
      auto* target_pattern = target->getTemplateInstantiationPattern();
      const auto target_pattern_location =
          target_pattern ? canonical_spelling_location(target_pattern) : std::nullopt;
      if (!target_pattern || !target_pattern->isFromASTFile() || !target_pattern_location ||
          *target_pattern_location != *pattern_location) {
        return nullptr;
      }
      if (auto* already = GetAlreadyImportedOrNull(source);
          already && already != target) {
        return nullptr;
      }
      MapImported(source, target);
      return target;
    }
    return target;
  }

  clang::FriendDecl* find_pch_equivalent_friend(clang::FriendDecl* source) {
    const auto source_location = canonical_spelling_location(source);
    auto* source_owner_decl = clang::Decl::castFromDeclContext(source->getDeclContext());
    auto* source_owner_record = llvm::dyn_cast<clang::CXXRecordDecl>(source_owner_decl);
    auto* source_owner = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
        source_owner_record);
    if (!source_owner_record ||
        (source_owner &&
         source_owner->getSpecializationKind() != clang::TSK_ImplicitInstantiation)) {
      return nullptr;
    }
    if (!source_location || !pch_headers_.count(source_location->first)) {
      return nullptr;
    }

    auto* target_owner = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
        GetAlreadyImportedOrNull(source_owner_record));
    if (source_owner) {
      auto* exact_target = import_pch_implicit_specialization(source_owner);
      if (!exact_target ||
          (target_owner && target_owner != exact_target)) {
        return nullptr;
      }
      target_owner = exact_target;
    } else {
      auto* equivalent = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
          find_pch_equivalent(source_owner_record));
      const auto source_owner_location = canonical_spelling_location(source_owner_record);
      const auto target_owner_location =
          equivalent ? canonical_spelling_location(equivalent) : std::nullopt;
      if (!equivalent || !equivalent->isFromASTFile() || !source_owner_location ||
          !target_owner_location || *source_owner_location != *target_owner_location) {
        return nullptr;
      }
      if (target_owner && target_owner != equivalent) {
        return nullptr;
      }
      target_owner = equivalent;
      if (!GetAlreadyImportedOrNull(source_owner_record)) {
        MapImported(source_owner_record, target_owner);
      }
    }
    auto* target_specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(target_owner);
    if (!target_owner ||
        (source_owner &&
         (!target_specialization ||
          !target_specialization->getSpecializedTemplate()->isFromASTFile()))) {
      return nullptr;
    }
    if (source_owner) {
      auto* source_pattern = source_owner->getTemplateInstantiationPattern();
      auto* target_pattern = target_owner->getTemplateInstantiationPattern();
      const auto source_pattern_location =
          source_pattern ? canonical_spelling_location(source_pattern) : std::nullopt;
      const auto target_pattern_location =
          target_pattern ? canonical_spelling_location(target_pattern) : std::nullopt;
      if (!source_pattern_location || !target_pattern || !target_pattern->isFromASTFile() ||
          !target_pattern_location ||
          *source_pattern_location != *target_pattern_location) {
        return nullptr;
      }
    }

    for (auto* target_friend : target_owner->friends()) {
      if (target_friend->getLexicalDeclContext() != target_owner ||
          (target_friend->getFriendType() != nullptr) !=
              (source->getFriendType() != nullptr) ||
          target_friend->isPackExpansion() != source->isPackExpansion() ||
          !target_owner->containsDeclAndLoad(target_friend)) {
        continue;
      }
      const auto target_location = canonical_spelling_location(target_friend);
      if (target_location && *target_location == *source_location) {
        return target_friend;
      }
    }
    return nullptr;
  }

protected:
  llvm::Expected<clang::Decl*> ImportImpl(clang::Decl* decl) override {
    if (auto* source = llvm::dyn_cast<clang::RequiresExprBodyDecl>(decl)) {
      // Requires-expression bodies are scoped under the function whose
      // constraints they evaluate. Importing that parent can revisit this body
      // before ASTImporter creates its usual placeholder. Install a child-AST
      // shell first so the recursive parent import can resolve it.
      auto location = Import(source->getLocation());
      if (!location) {
        return location.takeError();
      }
      auto* target = clang::RequiresExprBodyDecl::Create(
          getToContext(), getToContext().getTranslationUnitDecl(), *location);
      target->setImplicit(source->isImplicit());
      // ImportDeclContext may re-enter this declaration while importing its
      // parent function and will detach it from its provisional context.
      getToContext().getTranslationUnitDecl()->addDeclInternal(target);
      MapImported(source, target);

      auto context = ImportContext(source->getDeclContext());
      if (!context) {
        return context.takeError();
      }
      auto lexical_context = ImportContext(source->getLexicalDeclContext());
      if (!lexical_context) {
        return lexical_context.takeError();
      }
      auto* provisional_context = getToContext().getTranslationUnitDecl();
      if (target->getLexicalDeclContext() == provisional_context) {
        provisional_context->removeDecl(target);
      }
      if (target->getDeclContext() != *context) {
        target->setDeclContext(*context);
      }
      target->setLexicalDeclContext(*lexical_context);
      (*lexical_context)->addDeclInternal(target);
      return target;
    }
    if (auto* specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
      if (auto* imported = import_pch_implicit_specialization(specialization)) {
        return imported;
      }
    }
    if (auto* source_friend = llvm::dyn_cast<clang::FriendDecl>(decl)) {
      if (auto* existing = find_pch_equivalent_friend(source_friend)) {
        MapImported(decl, existing);
        return existing;
      }
    }
    auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
    if (!space) {
      if (auto* named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
        if (auto* existing = find_pch_equivalent(named)) {
          MapImported(decl, existing);
          return existing;
        }
      }
      return ASTImporter::ImportImpl(decl);
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
      if (!target) {
        const auto key = std::make_pair(static_cast<const clang::DeclContext*>(*context),
                                        clang::DeclarationName(identifier));
        if (pch_reader_ && pending_namespace_lookups_.insert(key).second) {
          pch_reader_->FindExternalVisibleDeclsByName(*context, clang::DeclarationName(identifier),
                                                     *context);
          for (auto* candidate : (*context)->noload_lookup(clang::DeclarationName(identifier))) {
            if ((target = llvm::dyn_cast<clang::NamespaceDecl>(candidate))) {
              break;
            }
          }
          pending_namespace_lookups_.erase(key);
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

public:
  void Imported(clang::Decl* from, clang::Decl*) override {
    auto* named = llvm::dyn_cast_or_null<clang::NamedDecl>(from);
    if (named && !llvm::isa<clang::UsingDirectiveDecl>(named) &&
        named->getDeclContext()->isTranslationUnit() && !named->getDeclName().isEmpty()) {
      imported_global_names_.insert(named->getDeclName());
    }
  }

  SessionImporter(clang::CompilerInstance& child, clang::CompilerInstance& parent,
                  const std::set<std::string>& pch_headers)
      // Seed an empty index, then let imports populate it. Indexing the real TU
      // here would deserialize the entire PCH before the first name lookup.
      // PCH declarations are resolved by find_pch_equivalent above.
      : ASTImporter(child.getASTContext(), child.getFileManager(), parent.getASTContext(),
                    parent.getFileManager(), false,
                    std::make_shared<clang::ASTImporterSharedState>(
                        *clang::TranslationUnitDecl::Create(child.getASTContext())))
      , pch_headers_(pch_headers)
      , pch_reader_(child.getASTReader().get()) {}

  clang::DeclarationName name_in(clang::DeclarationName name, clang::ASTContext& context) const {
    switch (name.getNameKind()) {
    case clang::DeclarationName::Identifier:
      return clang::DeclarationName(&context.Idents.get(name.getAsString()));
    case clang::DeclarationName::CXXOperatorName:
      return context.DeclarationNames.getCXXOperatorName(name.getCXXOverloadedOperator());
    case clang::DeclarationName::CXXLiteralOperatorName:
      return context.DeclarationNames.getCXXLiteralOperatorName(
          &context.Idents.get(name.getCXXLiteralIdentifier()->getName()));
    case clang::DeclarationName::CXXUsingDirective:
      return clang::DeclarationName::getUsingDirectiveName();
    default:
      return {};
    }
  }

  std::set<clang::DeclarationName> take_imported_global_names() {
    std::set<clang::DeclarationName> names;
    names.swap(imported_global_names_);
    return names;
  }
};

// Like clang-repl's completion importer, use a separate AST. Complete imports
// are needed for instantiated standard-library types (minimal imports conflict).
// Publish only current definitions and retain every overload. Version namespaces
// are implementation details; their current members also need ordinary lookup.
class SessionSource : public clang::ExternalSemaSource {
  SessionImporter importer_;
  const EditorPchCache::DeclSnapshot* decl_snapshot_;
  clang::CompilerInstance& parent_;
  clang::TranslationUnitDecl* child_;
  bool loaded_ = false;
  unsigned import_depth_ = 0;
  bool draining_global_overloads_ = false;
  std::map<const clang::DeclContext*, std::vector<clang::NamespaceDecl*>> namespaces_;
  std::vector<clang::NamespaceDecl*> version_namespaces_;
  std::set<const clang::DeclContext*> loaded_namespaces_;
  std::set<std::pair<const clang::DeclContext*, std::string>> pending_lookups_;
  std::set<clang::DeclarationName> drained_global_names_;

  bool already_in_pch_prefix(const clang::Decl* decl) const {
    return decl_snapshot_ && decl_snapshot_->count(decl) &&
           !llvm::isa<clang::NamespaceDecl, clang::UsingDirectiveDecl>(decl);
  }

  void drain_global_overloads() {
    if (draining_global_overloads_) {
      return;
    }
    draining_global_overloads_ = true;
    auto pending = importer_.take_imported_global_names();
    while (!pending.empty()) {
      const auto name = *pending.begin();
      pending.erase(pending.begin());
      if (!drained_global_names_.insert(name).second) {
        continue;
      }
      for (auto* decl : parent_.getASTContext().getTranslationUnitDecl()->getPrimaryContext()->lookup(
               name)) {
        import(decl);
      }
      auto newly_imported = importer_.take_imported_global_names();
      pending.insert(newly_imported.begin(), newly_imported.end());
    }
    draining_global_overloads_ = false;
  }

  void copy_session_macros(clang::Preprocessor& target,
                           const EditorPchCache::MacroSnapshot* macro_snapshot) {
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
      const auto definition = source.getMacroDefinition(id);
      // A visible module macro can change without changing the local directive
      // history. Skip replay only when both the captured and current definition
      // have no module-provided or ambiguous state.
      if (macro_snapshot && definition.getModuleMacros().empty() &&
          !definition.isAmbiguous()) {
        const auto captured = macro_snapshot->find(id);
        if (captured != macro_snapshot->end() && captured->second == latest) {
          continue;
        }
      }
      const auto* macro = definition.getMacroInfo();
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

  void define_record(clang::CXXRecordDecl* record) {
    if (!record || !record->hasDefinition()) {
      return;
    }
    auto imported = importer_.Import(record);
    if (imported) {
      if (auto* target = llvm::dyn_cast<clang::CXXRecordDecl>(*imported)) {
        if (auto* definition = target->getDefinition();
            definition && definition->isCompleteDefinition()) {
          return;
        }
      }
    } else {
      llvm::consumeError(imported.takeError());
    }
    if (auto error = importer_.ImportDefinition(record->getDefinition())) {
      llvm::consumeError(std::move(error));
    }
  }

  void define_type(clang::QualType type) {
    type = type.getNonReferenceType();
    while (!type.isNull() && type->isPointerType()) {
      type = type->getPointeeType();
    }
    if (!type.isNull()) {
      define_record(type->getAsCXXRecordDecl());
    }
  }

  void register_namespace(clang::NamespaceDecl* source, clang::NamespaceDecl* target) {
    if (!source || !target) {
      return;
    }
    auto* primary = target->getPrimaryContext();
    auto& sources = namespaces_[primary];
    auto* original = source->getCanonicalDecl();
    if (std::find(sources.begin(), sources.end(), original) == sources.end()) {
      sources.push_back(original);
    }
    primary->setHasExternalVisibleStorage(true);
  }

  void import(clang::Decl* decl) {
    ++import_depth_;
#if LLVM_VERSION_MAJOR >= 22
    const llvm::scope_exit finish([&] {
#else
    const auto finish = llvm::make_scope_exit([&] {
#endif
      if (--import_depth_ == 0) {
        drain_global_overloads();
      }
    });
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
      if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(visible)) {
        register_namespace(llvm::cast<clang::NamespaceDecl>(named), space);
      } else if (auto* source_using = llvm::dyn_cast<clang::UsingDirectiveDecl>(named)) {
        if (auto* target_using = llvm::dyn_cast<clang::UsingDirectiveDecl>(visible)) {
          register_namespace(source_using->getNominatedNamespace(),
                             target_using->getNominatedNamespace());
        }
      }
    }
    if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(named)) {
      define_type(value->getType());
    }
    if (auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(named)) {
      define_record(record);
    }
  }

public:
  SessionSource(clang::CompilerInstance& child, clang::CompilerInstance& parent,
                const std::set<std::string>& pch_headers,
                const EditorPchCache::MacroSnapshot* macro_snapshot,
                const EditorPchCache::DeclSnapshot* decl_snapshot)
      : importer_(child, parent, pch_headers)
      , decl_snapshot_(decl_snapshot)
      , parent_(parent)
      , child_(child.getASTContext().getTranslationUnitDecl()) {
    copy_session_macros(child.getPreprocessor(), macro_snapshot);
  }

  void initialize() {
    for (auto* unit = parent_.getASTContext().getTranslationUnitDecl(); unit;
         unit = unit->getPreviousDecl()) {
      for (auto* decl : unit->decls()) {
        if (llvm::isa<clang::UsingDirectiveDecl>(decl)) {
          import(decl);
        }
        if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
            space && space->getName().starts_with("__cpp_repl_version_")) {
          version_namespaces_.push_back(space);
        }
      }
    }
  }

  bool FindExternalVisibleDeclsByName(const clang::DeclContext* context,
                                      clang::DeclarationName name,
                                      const clang::DeclContext*) override {
    if (context->isTranslationUnit()) {
      const auto key = std::make_pair(context->getPrimaryContext(), name.getAsString());
      if (!pending_lookups_.insert(key).second) {
        return false;
      }
#if LLVM_VERSION_MAJOR >= 22
      const llvm::scope_exit cleanup([&] { pending_lookups_.erase(key); });
#else
      const auto cleanup = llvm::make_scope_exit([&] { pending_lookups_.erase(key); });
#endif
      bool any = false;
      const auto parent_name = importer_.name_in(name, parent_.getASTContext());
      if (!parent_name.isEmpty()) {
        for (auto* decl : parent_.getASTContext().getTranslationUnitDecl()->getPrimaryContext()->lookup(
                 parent_name)) {
          import(decl);
          any = true;
        }
        // Versioned public declarations are stored under private namespace
        // shells; check those shells by name while keeping obsolete versions
        // out of ordinary lookup.
        for (auto* version : version_namespaces_) {
          for (auto* member : version->lookup(parent_name)) {
            if (is_current_definition(*member)) {
              import(member);
              any = true;
            }
          }
        }
      }
      return any;
    }
    const auto found = namespaces_.find(context->getPrimaryContext());
    if (found == namespaces_.end()) {
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
    bool any = false;
    const auto sources = found->second;
    const auto parent_name = importer_.name_in(name, parent_.getASTContext());
    if (parent_name.isEmpty()) {
      return false;
    }
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
            if (!already_in_pch_prefix(decl)) {
              import(decl);
            }
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
        if (!already_in_pch_prefix(decl)) {
          import(decl);
        }
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
    if (decl && decl->getLocation().isValid() &&
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
    // Traversing the TU asks DeclContext::decls() for external lexical
    // declarations, which can deserialize the entire PCH. The already-loaded
    // roots include parser declarations and synthetic top-level statements.
    for (auto* decl : context.getTranslationUnitDecl()->noload_decls()) {
      visitor.TraverseDecl(decl);
    }
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
  bool collect_tokens_;
  const std::set<std::string>& pch_headers_;
  const EditorPchCache::MacroSnapshot* macro_snapshot_;
  const EditorPchCache::DeclSnapshot* decl_snapshot_;

public:
  EditorAction(clang::CompilerInstance& parent, EditorAnalysis& result, std::size_t size,
               bool collect_tokens,
               const std::set<std::string>& pch_headers,
               const EditorPchCache::MacroSnapshot* macro_snapshot,
               const EditorPchCache::DeclSnapshot* decl_snapshot)
      : parent_(parent)
      , result_(result)
      , size_(size)
      , collect_tokens_(collect_tokens)
      , pch_headers_(pch_headers)
      , macro_snapshot_(macro_snapshot)
      , decl_snapshot_(decl_snapshot) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
                                                        llvm::StringRef) override {
    if (collect_tokens_) {
      return std::make_unique<TokenConsumer>(result_, size_);
    }
    return std::make_unique<clang::ASTConsumer>();
  }

  void ExecuteAction() override {
    auto& child = getCompilerInstance();
    result_.pch_loaded = static_cast<bool>(child.getASTReader());
    auto source = llvm::makeIntrusiveRefCnt<SessionSource>(
        child, parent_, pch_headers_, result_.pch_loaded ? macro_snapshot_ : nullptr,
        result_.pch_loaded ? decl_snapshot_ : nullptr);
    if (collect_tokens_) {
      child.getPreprocessor().addPPCallbacks(std::make_unique<MacroExpansionColors>(
          child.getSourceManager(), child.getLangOpts(), result_, size_));
    }
#if LLVM_VERSION_MAJOR >= 22
    auto existing = child.getASTContext().getExternalSourcePtr();
    auto* existing_source = existing.get();
#else
    auto* existing_source = child.getASTContext().getExternalSource();
#endif
    if (auto* sema_source = llvm::dyn_cast_or_null<clang::ExternalSemaSource>(existing_source)) {
#if LLVM_VERSION_MAJOR >= 22
      auto multiplexed = llvm::makeIntrusiveRefCnt<clang::MultiplexExternalSemaSource>(
          llvm::IntrusiveRefCntPtr<clang::ExternalSemaSource>(sema_source), source);
#else
      auto multiplexed = llvm::makeIntrusiveRefCnt<clang::MultiplexExternalSemaSource>(
          sema_source, source.get());
#endif
      child.getASTContext().setExternalSource(multiplexed);
    } else {
      child.getASTContext().setExternalSource(source);
    }
    source->initialize();
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

class EditorDiagnostics : public clang::DiagnosticConsumer {
  EditorAnalysis& result_;

public:
  explicit EditorDiagnostics(EditorAnalysis& result) : result_(result) {}
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic& diagnostic) override {
    if (level >= clang::DiagnosticsEngine::Error && !result_.pch_loaded &&
        result_.pch_error.empty()) {
      llvm::SmallString<256> message;
      diagnostic.FormatDiagnostic(message);
      result_.pch_error = message.str().str();
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
                              bool completion, bool signatures_only,
                              const std::set<std::string>& pch_headers,
                              const EditorPchCache::MacroSnapshot* macro_snapshot,
                              const EditorPchCache::DeclSnapshot* decl_snapshot) {
  EditorAnalysis result;
  if (code.empty() || code.size() > 65536) {
    return result;
  }
  DiagnosticScope diagnostics(session.getDiagnostics());
  compiler->getDiagnostics().setClient(std::make_unique<EditorDiagnostics>(result).release(),
                                       true);
  const std::string filename = "input_line_[Editor]";
  auto buffer = llvm::MemoryBuffer::getMemBufferCopy(code + "\n;", filename);
  compiler->getPreprocessorOpts().addRemappedFile(filename, buffer.release());
  compiler->getFrontendOpts().Inputs = {clang::FrontendInputFile(filename, clang::Language::CXX)};
  compiler->getInvocation().getLangOpts().SpellChecking = false;
  if (completion) {
    const auto last_newline = code.rfind('\n');
    const unsigned line = std::count(code.begin(), code.end(), '\n') + 1;
    const unsigned column =
        code.size() - (last_newline == std::string::npos ? 0 : last_newline + 1) + 1;
    compiler->getFrontendOpts().CodeCompletionAt = {filename, line, column};
    compiler->setCodeCompletionConsumer(
        std::make_unique<CompletionConsumer>(result, session, code, signatures_only).release());
  }
  auto action =
      std::make_unique<EditorAction>(session, result, code.size(), !completion,
                                     pch_headers, macro_snapshot, decl_snapshot);
  llvm::CrashRecoveryContext::Enable();
  llvm::CrashRecoveryContext recovery;
  const bool completed = recovery.RunSafely([&] { compiler->ExecuteAction(*action); });
  llvm::CrashRecoveryContext::Disable();
  if (!completed) {
    // Clang may be partially torn down after a native crash. Avoid destructing
    // that compiler; the persistent interpreter has not parsed this buffer.
    (void)action.release();
    (void)compiler.release();
    EditorAnalysis failed;
    failed.pch_error = "editor analysis crashed while using the cached compiler";
    return failed;
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
