#include "definition_shadowing.hpp"

#include <map>
#include <mutex>
#include <set>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclContextInternals.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Sema.h"
#include "interpreter_undo.hpp"

namespace example {
namespace {
constexpr llvm::StringLiteral version_prefix = "__cpp_repl_version_";
class ShadowConsumer;
std::mutex consumers_mutex;
std::map<const clang::CompilerInstance *, ShadowConsumer *> consumers;

bool version_namespace(const clang::DeclContext *context) {
  const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(context);
  return space && space->getName().starts_with(version_prefix);
}

bool is_definition(const clang::NamedDecl *declaration) {
  if (!declaration)
    return false;
  if (llvm::isa<clang::ConceptDecl>(declaration))
    return true;
  if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
    return function->isThisDeclarationADefinition();
  if (auto *variable = llvm::dyn_cast<clang::VarDecl>(declaration))
    return variable->isThisDeclarationADefinition();
  if (auto *tag = llvm::dyn_cast<clang::TagDecl>(declaration))
    return tag->isThisDeclarationADefinition();
  if (auto *templ = llvm::dyn_cast<clang::TemplateDecl>(declaration))
    return is_definition(templ->getTemplatedDecl());
  return true;
}

bool has_nontrivial_temporary_cleanup(const clang::Stmt& statement) {
  if (const auto* bound = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(&statement)) {
    const auto* destructor = bound->getTemporary()->getDestructor();
    if (destructor && !destructor->isTrivial()) {
      return true;
    }
  }

  // A lambda body is not evaluated as part of evaluating the lambda
  // expression, so its local temporaries do not affect this full-expression.
  if (llvm::isa<clang::LambdaExpr>(&statement)) {
    return false;
  }

  for (const clang::Stmt* child : statement.children()) {
    if (child && has_nontrivial_temporary_cleanup(*child)) {
      return true;
    }
  }
  return false;
}

bool safe_to_capture_expression(const clang::Expr& expression) {
  if (expression.containsErrors() || expression.isTypeDependent() ||
      expression.isValueDependent()) {
    return false;
  }

  const clang::QualType type = expression.getType();
  if (type.isNull() || type->isDependentType() || type->isVoidType()) {
    return false;
  }

  // Clang's value-capture rewrite unwraps ExprWithCleanups. If any evaluated
  // subexpression owns a nontrivial temporary cleanup, capturing can drop that
  // cleanup even when the overall result is a scalar.
  if (has_nontrivial_temporary_cleanup(expression)) {
    return false;
  }

  // Keep this direct-result check as well: record prvalues can need destruction
  // even when Clang did not wrap them in CXXBindTemporaryExpr.
  if (expression.isPRValue()) {
    if (const auto* record = type->getAsCXXRecordDecl()) {
      if (record->hasNonTrivialDestructor()) {
        return false;
      }
    }
  }

  return true;
}

class ShadowConsumer : public clang::ASTConsumer {
  clang::CompilerInstance &compiler_;
  unsigned generation_ = 0;
  struct Hidden {
    clang::DeclContext *context;
    clang::NamedDecl *declaration;
    bool on_scope_chain;
  };
  std::vector<Hidden> hidden_;
  std::set<std::string> version_files_;
  bool capture_semicolon_expressions_ = false;

  void hide(clang::DeclContext *context, clang::NamedDecl *old) {
    if (auto *map = context->getPrimaryContext()->getLookupPtr()) {
      auto found = map->find(old->getDeclName());
      if (found == map->end())
        return;
      auto values = found->second.getLookupResult();
      if (!llvm::is_contained(values, old))
        return;
      found->second.remove(old);
      if (found->second.isNull())
        map->erase(found);
      auto &sema = compiler_.getSema();
      bool on_scope_chain = false;
      // Directives participate in context lookup, but their special name
      // has no FETokenInfo and must never reach IdentifierResolver.
      if (!llvm::isa<clang::UsingDirectiveDecl>(old)) {
        for (auto it = sema.IdResolver.begin(old->getDeclName());
             it != sema.IdResolver.end(); ++it)
          if (*it == old) {
            on_scope_chain = true;
            break;
          }
        if (on_scope_chain)
          sema.IdResolver.RemoveDecl(old);
        if (auto *scope = sema.getScopeForContext(context))
          scope->RemoveDecl(old);
      }
      hidden_.push_back({context, old, on_scope_chain});
    }
  }

  void shadow(clang::Decl *declaration) {
    if (auto *space = llvm::dyn_cast<clang::NamespaceDecl>(declaration)) {
      if (space->isImplicit() || space->getName().starts_with("__"))
        return;
      // Relocation appends namespaces to this context, so snapshot its
      // children.
      std::vector<clang::Decl *> children(space->decls_begin(),
                                          space->decls_end());
      for (auto *child : children)
        shadow(child);
      return;
    }
    auto *named = llvm::dyn_cast<clang::NamedDecl>(declaration);
    if (!named || named->isInvalidDecl() || named->getDeclName().isEmpty() ||
        !named->getDeclContext()->isFileContext() ||
        named->getLexicalDeclContext() != named->getDeclContext() ||
        llvm::isa<clang::UsingDecl, clang::UsingDirectiveDecl>(named))
      return;
    if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(named))
      if (function->isTemplateInstantiation() ||
          function->isFunctionTemplateSpecialization())
        return;
    if (llvm::isa<clang::ClassTemplateSpecializationDecl,
                  clang::VarTemplateSpecializationDecl>(named))
      return;
    if (!llvm::is_contained(named->getLexicalDeclContext()->decls(), named))
      return;
    auto &context = compiler_.getASTContext();
    auto &manager = context.getSourceManager();
    auto location = manager.getSpellingLoc(named->getLocation());
    if (named->getNameAsString().rfind("__", 0) == 0 ||
        manager.isInSystemHeader(location))
      return;
    const bool prompt =
        manager.getBufferName(location).starts_with("input_line_");
    auto *unit = named->getDeclContext();
    auto &sema = compiler_.getSema();
    if (!is_definition(named)) {
      // A later extern/forward declaration refers to the current definition;
      // it must not introduce an ambiguous entity beside its inline namespace.
      clang::LookupResult previous(sema, named->getDeclName(),
                                   named->getLocation(),
                                   clang::Sema::LookupOrdinaryName);
      previous.suppressDiagnostics();
      sema.LookupQualifiedName(previous, unit);
      for (auto *old : previous) {
        if (old == named || !version_namespace(old->getDeclContext()) ||
            !is_definition(old) || old->getKind() != named->getKind())
          continue;
        if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(named)) {
          if (sema.IsOverload(function, llvm::cast<clang::FunctionDecl>(old),
                              false))
            continue;
        } else if (auto *value = llvm::dyn_cast<clang::ValueDecl>(named)) {
          if (!context.hasSameType(
                  value->getType(),
                  llvm::cast<clang::ValueDecl>(old)->getType()))
            continue;
        }
        unit->removeDecl(named);
        named->setInvalidDecl();
        unit->makeDeclVisibleInContext(old);
        break;
      }
      return;
    }
    if (!prompt && !version_files_.count(manager.getFilename(location).str()))
      return;
    auto *space = clang::NamespaceDecl::Create(
        context, unit, true, {}, {},
        &context.Idents.get(version_prefix.str() +
                            std::to_string(generation_++)),
        nullptr, false);
    unit->addDecl(space);
    named->getLexicalDeclContext()->removeDecl(named);
    named->setDeclContext(space);
    if (auto *templ = llvm::dyn_cast<clang::TemplateDecl>(named))
      if (auto *underlying = templ->getTemplatedDecl())
        underlying->setDeclContext(space);
    space->addDecl(named);
    clang::LookupResult previous(sema, named->getDeclName(),
                                 named->getLocation(),
                                 clang::Sema::LookupOrdinaryName);
    previous.suppressDiagnostics();
    sema.LookupQualifiedName(previous, unit);
    for (auto *old : previous) {
      if (old == named)
        continue;
      auto *new_function = llvm::dyn_cast<clang::FunctionDecl>(named);
      auto *old_function = llvm::dyn_cast<clang::FunctionDecl>(old);
      if (new_function && old_function &&
          sema.IsOverload(new_function, old_function, false))
        continue;
      if (auto *new_template =
              llvm::dyn_cast<clang::FunctionTemplateDecl>(named))
        if (auto *old_template =
                llvm::dyn_cast<clang::FunctionTemplateDecl>(old))
          if (sema.IsOverload(new_template->getTemplatedDecl(),
                              old_template->getTemplatedDecl(), false))
            continue;
      hide(unit, old);
      if (auto *enumeration = llvm::dyn_cast<clang::EnumDecl>(old))
        if (!enumeration->isScoped())
          for (auto *value : enumeration->enumerators())
            hide(unit, value);
    }
  }

public:
  explicit ShadowConsumer(clang::CompilerInstance &compiler)
      : compiler_(compiler) {
    std::lock_guard<std::mutex> lock(consumers_mutex);
    consumers[&compiler] = this;
  }
  ~ShadowConsumer() override {
    std::lock_guard<std::mutex> lock(consumers_mutex);
    consumers.erase(&compiler_);
  }
  void finish(bool committed) {
    if (!committed)
      for (auto entry = hidden_.rbegin(); entry != hidden_.rend(); ++entry) {
        entry->context->makeDeclVisibleInContext(entry->declaration);
        if (entry->on_scope_chain) {
          auto &sema = compiler_.getSema();
          sema.IdResolver.AddDecl(entry->declaration);
          if (sema.TUScope)
            sema.TUScope->AddDecl(entry->declaration);
        }
      }
    hidden_.clear();
  }
  void version_files(const std::vector<std::string> &files) {
    version_files_ = {files.begin(), files.end()};
  }

  void capture_semicolon_expressions(bool capture) { capture_semicolon_expressions_ = capture; }
  void hide_files(const clang::DeclContext &context,
                  const std::set<std::string> &files) {
    auto &manager = compiler_.getSourceManager();
    for (auto *declaration : context.decls()) {
      if (auto *space = llvm::dyn_cast<clang::NamespaceDecl>(declaration)) {
        hide_files(*space, files);
        continue;
      }
      auto *named = llvm::dyn_cast<clang::NamedDecl>(declaration);
      if (!named || named->getDeclName().isEmpty() ||
          !is_current_definition(*named) ||
          !named->getDeclContext()->isFileContext() ||
          !files.count(
              manager.getFilename(manager.getExpansionLoc(named->getLocation()))
                  .str()))
        continue;
      auto *parent = named->getDeclContext();
      if (version_namespace(parent))
        parent = parent->getParent();
      hide(parent, named);
      if (auto *enumeration = llvm::dyn_cast<clang::EnumDecl>(named))
        if (!enumeration->isScoped())
          for (auto *value : enumeration->enumerators())
            hide(parent, value);
    }
  }
  bool HandleTopLevelDecl(clang::DeclGroupRef declarations) override {
    // A directive can be parsed after the last error diagnostic. Remove it
    // before IncrementalParser performs its internal error cleanup too.
    if (compiler_.getDiagnostics().hasErrorOccurred())
      detach_using_directives(compiler_.getASTContext().getTranslationUnitDecl(), true);
    for (auto* declaration : declarations) {
      if (capture_semicolon_expressions_ && !compiler_.getDiagnostics().hasErrorOccurred()) {
        auto* statement = llvm::dyn_cast<clang::TopLevelStmtDecl>(declaration);
        auto* expression = statement && statement->getStmt()
                               ? llvm::dyn_cast<clang::Expr>(statement->getStmt())
                               : nullptr;
        if (expression && safe_to_capture_expression(*expression) &&
            compiler_.getSourceManager()
                .getBufferName(
                    compiler_.getSourceManager().getExpansionLoc(statement->getBeginLoc()))
                .starts_with("input_line_")) {
          // ParseAndExecute emits the PTU before returning, so this capture
          // policy must be applied while the declaration is delivered here.
          statement->setSemiMissing();
        }
      }
      shadow(declaration);
    }
    return true;
  }
};

class ShadowAction : public clang::PluginASTAction {
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef) override {
    return std::make_unique<ShadowConsumer>(compiler);
  }
  bool ParseArgs(const clang::CompilerInstance &,
                 const std::vector<std::string> &) override {
    return true;
  }
  ActionType getActionType() override { return CmdlineBeforeMainAction; }
};
clang::FrontendPluginRegistry::Add<ShadowAction>
    registration("cpp-repl-shadowing", "version interactive definitions");
} // namespace

void enable_definition_shadowing(clang::CompilerInstance &compiler) {
  compiler.getFrontendOpts().AddPluginActions.push_back("cpp-repl-shadowing");
}

void capture_semicolon_expressions(clang::CompilerInstance& compiler, bool capture) {
  std::lock_guard<std::mutex> lock(consumers_mutex);
  if (auto found = consumers.find(&compiler); found != consumers.end()) {
    found->second->capture_semicolon_expressions(capture);
  }
}

std::string unversioned_name(std::string name) {
  std::size_t start = 0;
  while ((start = name.find(version_prefix.str(), start)) !=
         std::string::npos) {
    auto end = start + version_prefix.size();
    while (end < name.size() && name[end] >= '0' && name[end] <= '9')
      ++end;
    if (name.compare(end, 2, "::") == 0)
      name.erase(start, end + 2 - start);
    else
      start = end;
  }
  return name;
}

bool is_current_definition(const clang::NamedDecl &declaration) {
  const clang::NamedDecl *current = &declaration;
  for (auto *parent = current->getDeclContext(); parent;
       parent = parent->getParent()) {
    // Anonymous namespaces have no lookup name, but their members still do.
    if (parent->isFileContext() && !current->getDeclName().isEmpty()) {
      const clang::NamedDecl *identity = current;
      if (auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(current))
        if (auto *templ = record->getDescribedClassTemplate())
          identity = templ;
      if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(current))
        if (auto *templ = function->getDescribedFunctionTemplate())
          identity = templ;
      if (auto *variable = llvm::dyn_cast<clang::VarDecl>(current))
        if (auto *templ = variable->getDescribedVarTemplate())
          identity = templ;
      auto *lookup_context =
          version_namespace(parent) ? parent->getParent() : parent;
      auto visible = lookup_context->lookup(identity->getDeclName());
      if (!llvm::any_of(visible, [&](const auto *candidate) {
            return candidate->getCanonicalDecl() ==
                   identity->getCanonicalDecl();
          }))
        return false;
    }
    if (!version_namespace(parent))
      if (auto *named = llvm::dyn_cast<clang::NamedDecl>(parent))
        current = named;
    if (parent->isTranslationUnit())
      break;
  }
  return true;
}

DefinitionTransaction::DefinitionTransaction(clang::CompilerInstance &compiler)
    : compiler_(compiler) {}
DefinitionTransaction::~DefinitionTransaction() {
  std::lock_guard<std::mutex> lock(consumers_mutex);
  if (auto found = consumers.find(&compiler_); found != consumers.end())
    found->second->finish(committed_);
}
void DefinitionTransaction::commit() { committed_ = true; }

void version_source_files(clang::CompilerInstance &compiler,
                          const std::vector<std::string> &files) {
  std::lock_guard<std::mutex> lock(consumers_mutex);
  if (auto found = consumers.find(&compiler); found != consumers.end())
    found->second->version_files(files);
}
void hide_source_definitions(clang::CompilerInstance &compiler,
                             const std::vector<std::string> &files) {
  std::lock_guard<std::mutex> lock(consumers_mutex);
  if (auto found = consumers.find(&compiler); found != consumers.end())
    for (auto *unit :
         compiler.getASTContext().getTranslationUnitDecl()->redecls())
      found->second->hide_files(*unit, {files.begin(), files.end()});
}
} // namespace example
