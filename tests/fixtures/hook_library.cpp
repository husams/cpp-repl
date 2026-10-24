#include "hook_library.hpp"

#ifdef __linux__
extern "C" int hook_native_weak_missing() __attribute__((weak));
#endif

namespace hook_library {

int symbol_counter = 7;

int add(int a, int b) { return a + b; }
double add(double a, double b) { return a + b; }
int native_inc(int n) { return add(n, 1); }
int native_no_add(int n) { return n + 1; }
int native_optional() {
#ifdef __linux__
  return hook_native_weak_missing ? hook_native_weak_missing() : -1;
#else
  return -1;
#endif
}
int got_add(int a, int b) { return a + b; }
int Point::add(int n) const { return n + 1; }
int NativePoint::inc(int n) { return add(add(n, 1), 2); }
int NativePoint::run(int n) { return inc(n); }
int NativePoint::double_it(int n) { return add(n, n); }
int InlineNativePoint::run(int n) { return inc(n); }
int InlineNativePoint::double_it(int n) { return inline_add(n, n); }

}  // namespace hook_library
