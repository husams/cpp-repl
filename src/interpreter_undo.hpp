#pragma once

#include "clang/AST/DeclCXX.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/ADT/SmallVector.h"

namespace example {

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
  return interpreter.Undo();
}

}  // namespace example
