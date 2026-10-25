#pragma once
#include <string>
#include <vector>

namespace unresolved_demo {

class State {
  static long long total_;

 public:
  static long long read_total();
  static std::string title;
  static std::vector<int> samples;
  static const int weights[3];
};

bool verify();
const void* native_title_address();
const char* read_title();

}  // namespace unresolved_demo
