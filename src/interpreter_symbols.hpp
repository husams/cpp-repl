#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/Support/Error.h"

namespace llvm::orc {
class LLJIT;
}

namespace example {

// Clang's public lookup only matches exported symbols. Internal REPL lookups
// must also see hidden definitions in this interpreter's own JITDylib.
llvm::Expected<llvm::orc::ExecutorAddr> lookup_symbol(
    llvm::orc::LLJIT& jit, llvm::StringRef name);

}  // namespace example
