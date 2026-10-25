#pragma once

#include <string>
#include <vector>

namespace storage_demo {

inline long long initial_counter() { return 6 * 7; }

class State {
  static long long counter_;

 public:
  static long long current() { return counter_; }
  static std::string title;
  static std::vector<int> samples;
  static const int weights[3];
};

// This member already has storage in the precompiled example library.
struct Native {
  static int baseline;
};

}  // namespace storage_demo
