#include "hook_library.hpp"

namespace hook_library {

// Keep the callee in another translation unit so Clang emits GOT calls with
// -fno-plt, rather than using a direct call to a definition in this unit.
int got_inc(int n) { return got_add(n, 1) + 1; }
int got_double(int n) { return got_add(n, n); }

}  // namespace hook_library
