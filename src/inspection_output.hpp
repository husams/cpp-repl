#pragma once

#include <string>
#include <string_view>

namespace example {

bool inspection_colors_enabled();
std::string color_inspection(std::string_view text, bool enabled, bool names_only = false);

} // namespace example
