#pragma once

#include <string>

#include "replxx.hxx"

namespace example {

inline constexpr replxx::Replxx::Color kCommandColor = replxx::Replxx::Color::BRIGHTGREEN;
inline constexpr replxx::Replxx::Color kCommandErrorColor = replxx::Replxx::Color::BRIGHTRED;
inline constexpr replxx::Replxx::Color kPathColor = replxx::Replxx::Color::BRIGHTBLUE;
inline constexpr replxx::Replxx::Color kSeparatorColor = replxx::Replxx::Color::YELLOW;
inline constexpr replxx::Replxx::Color kClassNameColor = replxx::Replxx::Color::BRIGHTCYAN;

// replxx highlighter: '%' lines are colored by the command grammar (name,
// paths, ':' separators, class name, and the first grammar error in red);
// everything else
// is highlighted as C++.
void highlight_input(const std::string& input, replxx::Replxx::colors_t& colors);

}  // namespace example
