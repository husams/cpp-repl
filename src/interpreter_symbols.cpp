#include "interpreter_symbols.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"

namespace example {

llvm::Expected<llvm::orc::ExecutorAddr> lookup_symbol(
    llvm::orc::LLJIT& jit, llvm::StringRef name) {
  // Keep Clang's search order and exported-only platform/process lookups, but
  // allow hidden definitions in our own JIT. One lookup also preserves errors
  // materializing a JIT definition instead of falling back to a process symbol.
  auto order = llvm::orc::makeJITDylibSearchOrder(
      {&jit.getMainJITDylib(), jit.getPlatformJITDylib().get(),
       jit.getProcessSymbolsJITDylib().get()});
  order.front().second = llvm::orc::JITDylibLookupFlags::MatchAllSymbols;
  auto symbol = jit.getExecutionSession().lookup(order, jit.mangleAndIntern(name));
  if (!symbol) return symbol.takeError();
  return symbol->getAddress();
}

}  // namespace example
