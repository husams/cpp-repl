#include "state.hpp"

#include <iostream>

#if EXAMPLE_COMPILE_DB != 73
#error The compile database options must be loaded before this source.
#endif

namespace storage_demo {

bool check() {
  const bool scalar = State::current() == 42;
  const bool text = State::title == "startup storage";
  const bool vector = State::samples == std::vector<int>({3, 5, 8});
  const bool array = State::weights[0] == 2 && State::weights[1] == 4 &&
                     State::weights[2] == 6;
  const bool native = Native::baseline == 91;
  const bool options = EXAMPLE_COMPILE_DB == 73 && EXAMPLE_STARTUP_FLAG == 29;
  std::cout << "Private scalar: " << State::current() << '\n'
            << "String: " << State::title << '\n'
            << "Vector: " << State::samples[0] << ", " << State::samples[1]
            << ", " << State::samples[2] << '\n'
            << "Array: " << State::weights[0] << ", " << State::weights[1]
            << ", " << State::weights[2] << '\n'
            << "Native member: " << Native::baseline << '\n'
            << "Compile flags: " << EXAMPLE_COMPILE_DB << ", "
            << EXAMPLE_STARTUP_FLAG << '\n';
  const bool ok = scalar && text && vector && array && native && options;
  std::cout << (ok ? "STATIC_MEMBERS_PASS" : "STATIC_MEMBERS_FAIL") << '\n';
  return ok;
}

}  // namespace storage_demo
