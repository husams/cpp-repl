#include "inspection_output.hpp"

#include <cstdlib>
#include <sstream>

#include <unistd.h>

namespace example {

bool inspection_colors_enabled() {
  const auto* term = std::getenv("TERM");
  return isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR") &&
         (!term || std::string_view(term) != "dumb");
}

std::string color_inspection(std::string_view text, bool enabled, bool names_only) {
  if (!enabled) {
    return std::string(text);
  }
  std::istringstream input{std::string(text)};
  std::string result, line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      result += '\n';
      continue;
    }
    if (line.rfind("error:", 0) == 0) {
      result += "\033[1;31m" + line + "\033[0m\n";
    } else if (names_only) {
      result += "\033[36m" + line + "\033[0m\n";
    } else if (const auto split = line.find(" : "); split != std::string::npos) {
      result += "\033[36m" + line.substr(0, split) + "\033[0m : ";
      const auto value = line.substr(split + 3);
      const char* color = value.rfind("0x", 0) == 0 ? "\033[33m"
                          : value.rfind("no ", 0) == 0 || value.rfind("undefined", 0) == 0
                              ? "\033[33m"
                          : value.rfind("yes", 0) == 0 || value == "defined" ? "\033[32m"
                                                                             : "\033[37m";
      result += std::string(color) + value + "\033[0m\n";
    } else {
      result += "\033[1;36m" + line + "\033[0m\n";
    }
  }
  return result;
}

} // namespace example
