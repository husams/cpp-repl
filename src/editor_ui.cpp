#include "editor_ui.hpp"

#include <algorithm>
#include <string_view>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "cpp_lexer.hpp"
#include "highlight_repl.hpp"

namespace example {
using replxx::Replxx;

Replxx::Color semantic_color(SemanticKind kind) {
  switch (kind) {
  case SemanticKind::Type:
    return Replxx::Color::BRIGHTCYAN;
  case SemanticKind::Namespace:
    return Replxx::Color::BRIGHTBLUE;
  case SemanticKind::Function:
    return Replxx::Color::BRIGHTGREEN;
  case SemanticKind::Method:
    return Replxx::Color::GREEN;
  case SemanticKind::Field:
    return Replxx::Color::CYAN;
  case SemanticKind::Parameter:
    return Replxx::Color::YELLOW;
  case SemanticKind::EnumConstant:
    return Replxx::Color::BRIGHTMAGENTA;
  case SemanticKind::Macro:
    return Replxx::Color::BROWN;
  case SemanticKind::Keyword:
    return Replxx::Color::MAGENTA;
  case SemanticKind::Variable:
    return Replxx::Color::LIGHTGRAY;
  }
  return Replxx::Color::DEFAULT;
}

void highlight_editor_input(CppInterpreter& cpp, const std::string& input,
                            Replxx::colors_t& colors) {
  highlight_input(input, colors);
  std::string code = input;
  std::size_t offset = 0;
  if (is_command_line(input)) {
    const auto command = parse_command(input);
    if (!command.code || command.kind == CommandKind::Shell) {
      return; // Shell, paths, regex and commands keep their grammar colors.
    }
    code = command.code->value;
    offset = command.code->span.begin;
  }
  const auto lexical = lex_cpp(code);
  if (std::none_of(lexical.tokens.begin(), lexical.tokens.end(), [](const auto& token) {
        return token.kind == TokenKind::Identifier || token.kind == TokenKind::Type;
      })) {
    return;
  }
  const auto semantic = cpp.semantic_tokens(code);
  for (const auto& token : semantic) {
    // Semantic recovery must never paint comments, strings, escapes or brackets.
    const auto found =
        std::lower_bound(lexical.tokens.begin(), lexical.tokens.end(), token.begin,
                         [](const Token& item, std::size_t begin) { return item.begin < begin; });
    if (found == lexical.tokens.end() || found->begin != token.begin || found->end != token.end ||
        (found->kind != TokenKind::Identifier && found->kind != TokenKind::Type)) {
      continue;
    }
    const auto first = utf8_length(std::string_view(input).substr(0, offset + token.begin));
    const auto last = utf8_length(std::string_view(input).substr(0, offset + token.end));
    for (auto i = first; i < std::min(last, colors.size()); ++i) {
      colors[i] = semantic_color(token.kind);
    }
  }
}
} // namespace example
