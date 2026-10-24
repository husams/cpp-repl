#include "highlight_cpp.hpp"

#include <cstddef>

namespace example {

using replxx::Replxx;

Replxx::Color token_color(TokenKind kind, int depth) {
  switch (kind) {
    case TokenKind::Keyword:
      return Replxx::Color::MAGENTA;
    case TokenKind::Type:
      return Replxx::Color::BRIGHTCYAN;
    case TokenKind::String:
    case TokenKind::Char:
      return Replxx::Color::GREEN;
    case TokenKind::Escape:
      return Replxx::Color::YELLOW;
    case TokenKind::Number:
      return Replxx::Color::CYAN;
    case TokenKind::Comment:
      return Replxx::Color::GRAY;
    case TokenKind::Preprocessor:
      return Replxx::Color::BROWN;
    case TokenKind::Operator:
      return Replxx::Color::LIGHTGRAY;
    case TokenKind::Bracket: {
      static constexpr Replxx::Color kByDepth[] = {
          Replxx::Color::YELLOW, Replxx::Color::BRIGHTMAGENTA,
          Replxx::Color::BRIGHTBLUE};
      return kByDepth[static_cast<std::size_t>(depth) % 3];
    }
    case TokenKind::UnmatchedBracket:
      return Replxx::Color::BRIGHTRED;
    case TokenKind::Identifier:
    case TokenKind::Other:
      break;
  }
  return Replxx::Color::DEFAULT;
}

void highlight_cpp(const std::string& input, Replxx::colors_t& colors) {
  // Map each byte to the index of the code point it belongs to.
  std::vector<std::size_t> cp_of_byte(input.size());
  std::size_t cp = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if ((static_cast<unsigned char>(input[i]) & 0xC0) != 0x80 && i > 0) ++cp;
    cp_of_byte[i] = cp;
  }

  for (const Token& t : lex_cpp(input).tokens) {
    const Replxx::Color color = token_color(t.kind, t.depth);
    for (std::size_t i = t.begin; i < t.end; ++i) {
      if (cp_of_byte[i] < colors.size()) colors[cp_of_byte[i]] = color;
    }
  }
}

}  // namespace example
