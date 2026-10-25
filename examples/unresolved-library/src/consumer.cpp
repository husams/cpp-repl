#include "state.hpp"
#include <iostream>

// No static-member definitions in this library.
namespace unresolved_demo {

long long State::read_total() { return total_; }
const void* native_title_address() { return &State::title; }
const char* read_title() { return State::title.c_str(); }

bool verify() {
  const bool ok = State::read_total() == 42 &&
                  State::title == "provided at startup" &&
                  State::samples == std::vector<int>({3, 5, 8}) &&
                  State::weights[0] == 2 && State::weights[1] == 4 &&
                  State::weights[2] == 6;
  std::cout << "Native private scalar: " << State::read_total() << '\n'
            << "Native string: " << State::title << '\n'
            << "Native vector: " << State::samples[0] << ", "
            << State::samples[1] << ", " << State::samples[2] << '\n'
            << "Native array: " << State::weights[0] << ", "
            << State::weights[1] << ", " << State::weights[2] << '\n'
            << (ok ? "NATIVE_LIBRARY_PASS" : "NATIVE_LIBRARY_FAIL") << '\n';
  return ok;
}

}  // namespace unresolved_demo
