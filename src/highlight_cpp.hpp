#pragma once

#include <string>

#include "cpp_lexer.hpp"
#include "replxx.hxx"

namespace example {

// Color used for a token kind (`depth` selects the bracket color).
replxx::Replxx::Color token_color(TokenKind kind, int depth = 0);

// replxx highlighter callback: colors a whole (possibly multi-line) buffer.
// replxx indexes `colors` by code point, so UTF-8 input is handled.
void highlight_cpp(const std::string& input, replxx::Replxx::colors_t& colors);

}  // namespace example
