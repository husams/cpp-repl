#pragma once

#include <memory>

#include "clang/Basic/Diagnostic.h"

namespace example {

// Borrow or own a replacement client without changing the original client's
// ownership. Editor requests can also reset errors introduced by that request.
class DiagnosticClientScope {
  clang::DiagnosticsEngine& diagnostics_;
  clang::DiagnosticConsumer* previous_;
  std::unique_ptr<clang::DiagnosticConsumer> owned_;
  bool reset_errors_;

 public:
  DiagnosticClientScope(clang::DiagnosticsEngine& diagnostics,
                        clang::DiagnosticConsumer& replacement)
      : diagnostics_(diagnostics), previous_(diagnostics.getClient()),
        owned_(diagnostics.takeClient()), reset_errors_(false) {
    diagnostics_.setClient(&replacement, false);
  }
  DiagnosticClientScope(clang::DiagnosticsEngine& diagnostics,
                        std::unique_ptr<clang::DiagnosticConsumer> replacement,
                        bool reset_errors = false)
      : diagnostics_(diagnostics), previous_(diagnostics.getClient()),
        owned_(diagnostics.takeClient()),
        reset_errors_(reset_errors && !diagnostics.hasErrorOccurred()) {
    diagnostics_.setClient(replacement.release(), true);
  }
  ~DiagnosticClientScope() {
    if (owned_)
      diagnostics_.setClient(owned_.release(), true);
    else
      diagnostics_.setClient(previous_, false);
    if (reset_errors_)
      diagnostics_.Reset(true);
  }
  DiagnosticClientScope(const DiagnosticClientScope&) = delete;
  DiagnosticClientScope& operator=(const DiagnosticClientScope&) = delete;
};

} // namespace example
