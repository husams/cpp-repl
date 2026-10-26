#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace example {

inline constexpr std::array<std::string_view, 2> repl_option_names = {"print_values", "keep_hints"};

struct ReplOptions {
  bool print_values = true;
  bool keep_hints = true;
};

inline std::optional<bool> get_repl_option(const ReplOptions& options, std::string_view name) {
  if (name == "print_values") {
    return options.print_values;
  }
  if (name == "keep_hints") {
    return options.keep_hints;
  }
  return std::nullopt;
}

inline bool set_repl_option(ReplOptions& options, std::string_view name, bool value) {
  if (name == "print_values") {
    options.print_values = value;
  } else if (name == "keep_hints") {
    options.keep_hints = value;
  } else {
    return false;
  }
  return true;
}

} // namespace example
