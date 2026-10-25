#include "unresolved_callables.hpp"

namespace unresolved_callables {
int Worker::run(int x) const {
  return hidden(x);
}

int Worker::run_static(int x) {
  return scale(x);
}

extern "C" int callable_native_probe() {
  const auto v = transform(Value{1, 2, 3, 4}, 2.5);
  return adjust(3) == 10 && adjust(1.5) == 3.75 && Worker(40).run(2) == 42 &&
         Worker::run_static(7) == 21 && decorate("native") == "[native]" && v.a == 2.5 &&
         v.b == 5 && v.c == 7.5 && v.d == 10;
}
} // namespace unresolved_callables
