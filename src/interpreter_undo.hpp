#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
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

inline llvm::Expected<clang::PartialTranslationUnit&> parse_input(clang::Interpreter& interpreter,
                                                                  llvm::StringRef code) {
  auto parsed = interpreter.Parse(code);
  if (!parsed) {
    auto& context = interpreter.getASTContext();
    restore_lookup(context, context.getTranslationUnitDecl());
  }
  return parsed;
}

// Clang 21/22's CleanUpPTU asks using-directive names for FETokenInfo, which
// they do not have, and crashes. Anonymous namespaces add implicit directives.
// Detach top-level directives before Undo; nested ones are not visited by
// that cleanup loop. The TU must belong to the most recent input; nullptr is
// only for the known-empty input used to retrieve a pending expression value.
inline llvm::Error undo_input(clang::Interpreter& interpreter, clang::TranslationUnitDecl* tu) {
  if (tu) {
    llvm::SmallVector<clang::UsingDirectiveDecl*, 4> directives;
    for (clang::Decl* decl : tu->decls())
      if (auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl))
        directives.push_back(directive);
    for (auto* directive : directives)
      tu->removeDecl(directive);
  }
  auto error = interpreter.Undo();
  if (!error && tu) {
    restore_lookup(interpreter.getASTContext(), tu);
  }
  return error;
}

}  // namespace example
