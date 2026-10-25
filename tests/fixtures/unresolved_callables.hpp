#pragma once
#include <string>

namespace unresolved_callables {
struct Value {
  double a, b, c, d;
};

int adjust(int);
double adjust(double);
Value transform(Value, double);
std::string decorate(const std::string&);

class Worker {
  int value_;
  int hidden(int) const& noexcept;
  static int scale(int);

public:
  explicit Worker(int value)
      : value_(value) {}

  int run(int) const;
  static int run_static(int);
};

extern "C" int callable_native_probe();
} // namespace unresolved_callables
