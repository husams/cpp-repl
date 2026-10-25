#pragma once

#include <string>

namespace unresolved_storage_fixture {

class State {
  static long long value_;
  static std::string label_;

 public:
  static long long read();
  static const char* label();
  static const void* value_address();
};

}  // namespace unresolved_storage_fixture
