#pragma once

#include <mutex>

#include "frida-gum.h"

namespace example {

inline void initialize_gum() {
  static std::once_flag initialized;
  std::call_once(initialized, gum_init_embedded);
}

} // namespace example
