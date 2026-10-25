#pragma once

namespace hook_library {

int add(int a, int b);
double add(double a, double b);
int native_inc(int n);
int native_no_add(int n);
int native_optional();
extern int symbol_counter;
struct StaticStorage {
  static long long total;
  static const char* label;
};
#ifdef __linux__
int got_add(int a, int b);
int got_inc(int n);
int got_double(int n);
#endif

class Point {
  int add(int n) const;

 public:
  int inc(int n) const { return add(n); }
  int double_it(int n) const { return add(n) * 2; }
};

class NativePoint {
  int inc(int n);

 public:
  int run(int n);
  int double_it(int n);
};

[[gnu::used, gnu::visibility("default")]] inline int inline_add(int a, int b) { return a + b; }

class InlineNativePoint {
  [[gnu::used, gnu::visibility("default")]] int inc(int n) { return inline_add(n, 1); }

 public:
  int run(int n);
  int double_it(int n);
};

}  // namespace hook_library
