#pragma once

#include <string>

namespace specialized_method_demo {
template <class T>
struct Person {
  T value;
  std::string getName() const;
};

template <>
std::string Person<int>::getName() const;
}  // namespace specialized_method_demo
