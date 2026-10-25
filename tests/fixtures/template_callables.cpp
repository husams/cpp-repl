#include "template_callables.hpp"

extern "C" int template_callable_native_probe() {
  template_callables::Person<int> a{42};
  template_callables::Person<long> b{7};
  return a.name() == "int42" && b.name() == "long7" &&
         template_callables::describe<int>(9) == "function9";
}
