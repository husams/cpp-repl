#pragma once

#include <memory>

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/ADT/SmallVector.h"

namespace example {

// CleanUpPTU erases the entire lookup entry for a redeclared name (notably
// namespace std) when its most recent declaration belongs to a rejected input.
// Restore the previous declaration in the same primary context Clang cleaned.
inline void restore_lookup(clang::ASTContext& context, clang::TranslationUnitDecl* failed) {
  auto* primary = context.getTranslationUnitDecl()->getPrimaryContext();
  for (clang::Decl* decl : failed->decls()) {
    auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
    if (!named || named->getDeclName().isEmpty()) {
      continue;
    }
    for (auto* previous = decl->getPreviousDecl(); previous;
         previous = previous->getPreviousDecl()) {
      if (previous->getTranslationUnitDecl() == failed || previous->isInvalidDecl()) {
        continue;
      }
      auto* candidate = llvm::cast<clang::NamedDecl>(previous);
      bool visible = false;
      for (auto* existing : primary->lookup(named->getDeclName())) {
        visible |= existing->getCanonicalDecl() == candidate->getCanonicalDecl();
      }
      // An overload group can be partly intact. Restore each missing entity,
      // without replacing a declaration that survived the cleanup.
      if (!visible) {
        primary->makeDeclVisibleInContext(candidate);
      }
      break;
    }
    // A system header first included by the rejected input keeps its include
    // guard and its namespace contents in Clang's AST. Keep that namespace
    // reachable too; otherwise re-including <string> cannot recover std.
    // Do not publish declarations or namespaces introduced by rejected user code.
    if (primary->lookup(named->getDeclName()).empty() && llvm::isa<clang::NamespaceDecl>(named) &&
        !named->isInvalidDecl() &&
        context.getSourceManager().isInSystemHeader(named->getLocation())) {
      primary->makeDeclVisibleInContext(named);
    }
  }
}

// Clang 21/22's CleanUpPTU asks using-directive names for FETokenInfo, which
// they do not have. Only top-level directives are visited by that cleanup.
inline void detach_using_directives(clang::TranslationUnitDecl* tu) {
  llvm::SmallVector<clang::UsingDirectiveDecl*, 4> directives;
  for (clang::Decl* decl : tu->decls())
    if (auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl))
      directives.push_back(directive);
  for (auto* directive : directives)
    tu->removeDecl(directive);
}

// Parse cleans up internally before returning an error. Intercept diagnostics
// so directives preceding even a final #error are detached in time, while
// preserving the existing diagnostic client's output, counts and ownership.
class ParseDiagnostics : public clang::ForwardingDiagnosticConsumer {
  clang::ASTContext& context_;
  clang::TranslationUnitDecl* previous_unit_;
  clang::DiagnosticsEngine& diagnostics_;
  clang::DiagnosticConsumer* previous_;
  std::unique_ptr<clang::DiagnosticConsumer> previous_owner_;

 public:
  explicit ParseDiagnostics(clang::Interpreter& interpreter)
      : clang::ForwardingDiagnosticConsumer(
            *interpreter.getCompilerInstance()->getDiagnostics().getClient()),
        context_(interpreter.getASTContext()),
        previous_unit_(context_.getTranslationUnitDecl()),
        diagnostics_(interpreter.getCompilerInstance()->getDiagnostics()),
        previous_(diagnostics_.getClient()), previous_owner_(diagnostics_.takeClient()) {
    diagnostics_.setClient(this, false);
  }
  ~ParseDiagnostics() override {
    if (previous_owner_)
      diagnostics_.setClient(previous_owner_.release());
    else
      diagnostics_.setClient(previous_, false);
  }
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic& diagnostic) override {
    clang::ForwardingDiagnosticConsumer::HandleDiagnostic(level, diagnostic);
    if (level >= clang::DiagnosticsEngine::Error &&
        context_.getTranslationUnitDecl() != previous_unit_)
      detach_using_directives(context_.getTranslationUnitDecl());
  }
};

inline llvm::Expected<clang::PartialTranslationUnit&> parse_input(clang::Interpreter& interpreter,
                                                                  llvm::StringRef code) {
  ParseDiagnostics diagnostics(interpreter);
  auto parsed = interpreter.Parse(code);
  if (!parsed) {
    auto& context = interpreter.getASTContext();
    restore_lookup(context, context.getTranslationUnitDecl());
  }
  return parsed;
}

// Anonymous namespaces add implicit directives too. Detach before Undo as
// well as internal parse cleanup. The TU must be the most recent input; nullptr is
// only for the known-empty input used to retrieve a pending expression value.
inline llvm::Error undo_input(clang::Interpreter& interpreter, clang::TranslationUnitDecl* tu) {
  if (tu) {
    detach_using_directives(tu);
  }
  auto error = interpreter.Undo();
  if (!error && tu) {
    restore_lookup(interpreter.getASTContext(), tu);
  }
  return error;
}

}  // namespace example
