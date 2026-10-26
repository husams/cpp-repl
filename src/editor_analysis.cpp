#include "editor_analysis.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTImporter.h"
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
#include "cpp_symbols.hpp"
#include "definition_shadowing.hpp"
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
      item.label += param->getType().getAsString(policy);
      if (!param->getName().empty()) {
        item.label += " " + param->getNameAsString();
      }
      if (param->hasDefaultArg()) {
        item.label += " = ...";
      }
    }
    if (function->isVariadic()) {
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
  return item;
}

class CompletionConsumer : public clang::CodeCompleteConsumer {
  clang::GlobalCodeCompletionAllocator allocator_;
  clang::CodeCompletionTUInfo info_{std::make_shared<clang::GlobalCodeCompletionAllocator>()};
  EditorAnalysis& result_;
  clang::CompilerInstance& session_;
  const std::string& code_;

public:
  CompletionConsumer(EditorAnalysis& result, clang::CompilerInstance& session,
                     const std::string& code)
      : CodeCompleteConsumer([] {
        clang::CodeCompleteOptions options;
        options.IncludeGlobals = true;
        options.IncludeMacros = true;
        options.IncludeCodePatterns = true;
        options.IncludeBriefComments = true;
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
    const bool preprocessor_context =
        kind == clang::CodeCompletionContext::CCC_MacroName ||
        kind == clang::CodeCompletionContext::CCC_MacroNameUse ||
        kind == clang::CodeCompletionContext::CCC_PreprocessorExpression ||
        kind == clang::CodeCompletionContext::CCC_PreprocessorDirective ||
        kind == clang::CodeCompletionContext::CCC_IncludedFile;
    const bool macro_context = kind == clang::CodeCompletionContext::CCC_MacroName ||
                               kind == clang::CodeCompletionContext::CCC_MacroNameUse ||
                               kind == clang::CodeCompletionContext::CCC_PreprocessorExpression;
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
      } else if (preprocessor_context &&
                 candidate.Kind == clang::CodeCompletionResult::RK_Pattern) {
        auto* completion =
            candidate.CreateCodeCompletionString(sema, context, allocator_, info_, true);
        if (!completion || !completion->getTypedText()) {
          continue;
        }
        item.text = item.label = completion->getTypedText();
        item.kind = SemanticKind::Keyword;
        item.priority = completion->getPriority();
      } else {
        continue;
      }
      if (!llvm::StringRef(item.text).starts_with(prefix) ||
          (item.text.rfind("__", 0) == 0 &&
           !(macro_context && candidate.Kind == clang::CodeCompletionResult::RK_Macro))) {
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
    const auto before = llvm::StringRef(code_).drop_back(prefix.size()).rtrim();
    if (!context.getBaseType().isNull() || before.ends_with(".") || before.ends_with("->") ||
        before.ends_with("::")) {
      return;
    }
    if (context.getKind() == clang::CodeCompletionContext::CCC_Type && !before.empty()) {
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
      }
    }
  }
};

// Import namespace shells lazily, but complete the declarations actually used.
// Importing every namespace member eagerly would resurrect obsolete versions
// and make a standard-library include expensive on every editor request.
class SessionImporter : public clang::ASTImporter {
protected:
  llvm::Expected<clang::Decl*> ImportImpl(clang::Decl* decl) override {
    auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
    if (!space) {
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
  SessionImporter(clang::CompilerInstance& child, clang::CompilerInstance& parent)
      : ASTImporter(child.getASTContext(), child.getFileManager(), parent.getASTContext(),
                    parent.getFileManager(), false) {}
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

  void import(clang::Decl* decl) {
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
        auto* primary = space->getPrimaryContext();
        auto& sources = namespaces_[primary];
        auto* original = llvm::cast<clang::NamespaceDecl>(named);
        if (std::find(sources.begin(), sources.end(), original) == sources.end()) {
          sources.push_back(original);
        }
        primary->setHasExternalVisibleStorage(true);
      }
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
      : importer_(child, parent)
      , parent_(parent)
      , child_(child.getASTContext().getTranslationUnitDecl()) {
    copy_session_macros(child.getPreprocessor());
  }

  bool FindExternalVisibleDeclsByName(const clang::DeclContext* context,
                                      clang::DeclarationName name,
                                      const clang::DeclContext*) override {
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
    const llvm::scope_exit cleanup([&] { pending_lookups_.erase(key); });
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
  bool completion_;

public:
  EditorAction(clang::CompilerInstance& parent, EditorAnalysis& result, std::size_t size,
               bool completion)
      : parent_(parent)
      , result_(result)
      , size_(size)
      , completion_(completion) {}

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
    child.getASTContext().getTranslationUnitDecl()->setHasExternalVisibleStorage(true);
    child.getASTContext().getTranslationUnitDecl()->lookups();
    // Keep REPL top-level statements, but terminate this finite buffer with EOF.
    // LLVM 21/22's lexer emits annot_repl_input_end whenever the language's REPL
    // extensions are enabled, independently of IncrementalProcessing. Parser
    // recovery inside an unfinished body does not recognize that annotation.
    // Preprocessor::Lex passes its mutable result token to this observer through
    // a const reference; normalize only that token in our disposable compiler.
    child.getPreprocessor().setTokenWatcher([completion = completion_](const clang::Token& token) {
      if (!completion && token.is(clang::tok::annot_repl_input_end)) {
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
                              bool completion) {
  EditorAnalysis result;
  if (code.empty() || code.size() > 65536) {
    return result;
  }
  DiagnosticScope diagnostics(session.getDiagnostics());
  compiler->getDiagnostics().setClient(std::make_unique<clang::IgnoringDiagConsumer>().release(),
                                       true);
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
        std::make_unique<CompletionConsumer>(result, session, code).release());
  }
  auto action = std::make_unique<EditorAction>(session, result, code.size(), completion);
  llvm::CrashRecoveryContext::Enable();
  llvm::CrashRecoveryContext recovery;
  const bool completed = recovery.RunSafely([&] { compiler->ExecuteAction(*action); });
  llvm::CrashRecoveryContext::Disable();
  if (!completed) {
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
  return result;
}
} // namespace example
