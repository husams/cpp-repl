#pragma once

#include <string>

namespace class_instance_demo {
template <class T>
struct Person {
  T value;
  std::string getName() const;
};
}  // namespace class_instance_demo
