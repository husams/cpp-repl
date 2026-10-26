#include "editor_input.hpp"

#include <algorithm>
#include <cctype>

#include "commands.hpp"
#include "cpp_lexer.hpp"

namespace example {

namespace {

char matching_close(char32_t key) {
  switch (key) {
  case '(':
    return ')';
  case '[':
    return ']';
  case '{':
    return '}';
  case '"':
    return '"';
  case '\'':
    return '\'';
  default:
    return 0;
  }
}

bool literal_or_comment(TokenKind kind) {
  return kind == TokenKind::String || kind == TokenKind::Char || kind == TokenKind::Escape ||
         kind == TokenKind::Comment;
}

bool escaped_at(std::string_view text, std::size_t pos) {
  std::size_t slashes = 0;
  while (pos > 0 && text[pos - 1] == '\\') {
    --pos;
    ++slashes;
  }
  return slashes % 2 != 0;
}

bool in_preprocessor_quote(std::string_view code, std::size_t cursor_byte) {
  cursor_byte = std::min(cursor_byte, code.size());
  auto begin = cursor_byte == 0 ? std::string_view::npos : code.rfind('\n', cursor_byte - 1);
  begin = begin == std::string_view::npos ? 0 : begin + 1;
  while (begin > 1 && code[begin - 2] == '\\') {
    const auto previous_newline = begin - 1;
    const auto prior =
        previous_newline == 0 ? std::string_view::npos : code.rfind('\n', previous_newline - 1);
    begin = prior == std::string_view::npos ? 0 : prior + 1;
  }

  const auto physical_end = code.find('\n', begin);
  const auto first_line = code.substr(
      begin, (physical_end == std::string_view::npos ? code.size() : physical_end) - begin);
  const auto first = first_line.find_first_not_of(" \t");
  if (first == std::string_view::npos || first_line[first] != '#') {
    return false;
  }

  bool quoted = false;
  bool character = false;
  bool escaped = false;
  bool block_comment = false;
  bool line_comment = false;
  for (std::size_t i = begin; i < cursor_byte; ++i) {
    const char ch = code[i];
    if (ch == '\\' && i + 1 < cursor_byte && code[i + 1] == '\n') {
      ++i;
      continue;
    }
    if (line_comment) {
      if (ch == '\n') {
        line_comment = false;
      }
      continue;
    }
    if (block_comment) {
      if (ch == '*' && i + 1 < cursor_byte && code[i + 1] == '/') {
        block_comment = false;
        ++i;
      }
      continue;
    }
    if (quoted || character) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if ((quoted && ch == '"') || (character && ch == '\'')) {
        quoted = false;
        character = false;
      }
      continue;
    }
    if (ch == '/' && i + 1 < cursor_byte && code[i + 1] == '*') {
      block_comment = true;
      ++i;
    } else if (ch == '/' && i + 1 < cursor_byte && code[i + 1] == '/') {
      line_comment = true;
      ++i;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '\'') {
      character = true;
    }
  }
  return quoted;
}

enum class IncludeContext { None, BeforeHeader, InsideAngleHeader };

IncludeContext include_context(std::string_view code, std::size_t cursor_byte) {
  const auto prefix = code.substr(0, cursor_byte);
  const auto lexed = lex_cpp(prefix);
  if (lexed.tokens.empty() || lexed.tokens.back().kind != TokenKind::Preprocessor ||
      lexed.tokens.back().end != cursor_byte) {
    return IncludeContext::None;
  }
  const auto directive = prefix.substr(lexed.tokens.back().begin);
  std::size_t pos = 1; // The lexer has verified the leading '#'.
  const auto skip_trivia = [&] {
    while (pos < directive.size()) {
      if (std::isspace(static_cast<unsigned char>(directive[pos])) != 0) {
        ++pos;
      } else if (directive.substr(pos, 2) == "\\\n") {
        pos += 2;
      } else if (directive.substr(pos, 2) == "/*") {
        const auto end = directive.find("*/", pos + 2);
        if (end == std::string_view::npos) {
          return false;
        }
        pos = end + 2;
      } else {
        break;
      }
    }
    return true;
  };
  if (!skip_trivia()) {
    return IncludeContext::None;
  }
  const auto name_begin = pos;
  while (pos < directive.size() &&
         (std::isalnum(static_cast<unsigned char>(directive[pos])) != 0 || directive[pos] == '_')) {
    ++pos;
  }
  const auto name = directive.substr(name_begin, pos - name_begin);
  if ((name != "include" && name != "include_next" && name != "import") || !skip_trivia()) {
    return IncludeContext::None;
  }
  if (pos == directive.size()) {
    return IncludeContext::BeforeHeader;
  }
  if (directive[pos] == '<' && directive.find('>', pos + 1) == std::string_view::npos) {
    return IncludeContext::InsideAngleHeader;
  }
  return IncludeContext::None;
}

struct Payload {
  std::string_view code;
  std::size_t cursor = 0;
  bool valid = false;
};

Payload cpp_payload(std::string_view buffer, std::size_t cursor_byte) {
  cursor_byte = std::min(cursor_byte, buffer.size());
  if (!is_command_line(buffer)) {
    return {buffer, cursor_byte, true};
  }

  const ParsedCommand command = parse_command(buffer);
  if (command.kind != CommandKind::Mock && command.kind != CommandKind::Pre &&
      command.kind != CommandKind::Post && command.kind != CommandKind::DefineVariable) {
    return {};
  }
  if (!command.code || cursor_byte < command.code->span.begin ||
      cursor_byte > command.code->span.end) {
    return {};
  }
  return {
      buffer.substr(command.code->span.begin, command.code->span.end - command.code->span.begin),
      cursor_byte - command.code->span.begin, true};
}

bool in_literal_or_comment(std::string_view code, std::size_t cursor_byte) {
  const auto lexed = lex_cpp(code.substr(0, std::min(cursor_byte, code.size())));
  for (auto it = lexed.tokens.rbegin(); it != lexed.tokens.rend(); ++it) {
    if (it->end != std::min(cursor_byte, code.size())) {
      continue;
    }
    if (it->kind == TokenKind::Preprocessor) {
      return in_preprocessor_quote(code, cursor_byte);
    }
    return literal_or_comment(it->kind) &&
           (lexed.unterminated ||
            (it->kind == TokenKind::Comment && code.substr(it->begin, 2) == "//"));
  }
  return false;
}

bool raw_string_prefix_before(std::string_view code, std::size_t cursor_byte) {
  const auto prefix = code.substr(0, std::min(cursor_byte, code.size()));
  const auto lexed = lex_cpp(prefix);
  if (lexed.tokens.empty()) {
    return false;
  }
  const auto& token = lexed.tokens.back();
  if (token.kind != TokenKind::Identifier || token.end != prefix.size()) {
    return false;
  }
  const auto word = prefix.substr(token.begin, token.end - token.begin);
  return word == "R" || word == "u8R" || word == "uR" || word == "UR" || word == "LR";
}

bool number_ends_at(std::string_view code, std::size_t cursor_byte) {
  const auto prefix = code.substr(0, std::min(cursor_byte, code.size()));
  const auto lexed = lex_cpp(prefix);
  return !lexed.tokens.empty() && lexed.tokens.back().kind == TokenKind::Number &&
         lexed.tokens.back().end == prefix.size();
}

bool incomplete_at_cursor(std::string_view code, std::size_t cursor_byte) {
  cursor_byte = std::min(cursor_byte, code.size());
  // Auto-pairing puts the cursor between the two tokens. In this position,
  // consider the opener together with its left-hand expression so `21 * ()`
  // continues while a complete `f()` still submits.
  std::size_t opening = cursor_byte;
  while (opening > 0 && std::isspace(static_cast<unsigned char>(code[opening - 1])) != 0) {
    --opening;
  }
  std::size_t closing = cursor_byte;
  while (closing < code.size() && std::isspace(static_cast<unsigned char>(code[closing])) != 0) {
    ++closing;
  }
  if (opening > 0 && closing < code.size() && code[opening - 1] == '(' && code[closing] == ')') {
    const auto before_opener = code.substr(0, opening - 1);
    const auto before_tokens = lex_cpp(before_opener).tokens;
    const bool has_expression =
        std::any_of(before_tokens.begin(), before_tokens.end(),
                    [](const Token& token) { return token.kind != TokenKind::Comment; });
    if (is_incomplete(before_opener) || !has_expression) {
      return true;
    }
  }
  // If the cursor sits before an editor-inserted quote that the user has just
  // escaped, Enter must insert the continued line before that quote. A cursor
  // elsewhere inside a valid quoted token (including an include path) does
  // not need special continuation handling.
  if (cursor_byte < code.size() && code[cursor_byte] == '"' && escaped_at(code, cursor_byte) &&
      in_preprocessor_quote(code, cursor_byte)) {
    return true;
  }

  const auto prefix = code.substr(0, cursor_byte);
  const auto prefix_tokens = lex_cpp(prefix).tokens;
  for (auto it = prefix_tokens.rbegin(); it != prefix_tokens.rend(); ++it) {
    if (it->kind == TokenKind::Comment) {
      continue;
    }
    if (it->kind == TokenKind::Operator) {
      const auto op = prefix.substr(it->begin, it->end - it->begin);
      if (op != "++" && op != "--" && op != ";") {
        return true;
      }
    }
    break;
  }

  if (cursor_byte < code.size() && code[cursor_byte] == '}') {
    const auto lexed = lex_cpp(code);
    for (const auto& token : lexed.tokens) {
      if (token.begin == cursor_byte && token.kind == TokenKind::Bracket &&
          code[token.begin] == '}') {
        return true;
      }
    }
  }
  return false;
}

} // namespace

std::size_t utf8_byte_offset(std::string_view text, std::size_t codepoint_offset) {
  std::size_t bytes = 0;
  while (bytes < text.size() && codepoint_offset > 0) {
    const unsigned char lead = static_cast<unsigned char>(text[bytes]);
    bytes += lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : 4;
    --codepoint_offset;
  }
  return std::min(bytes, text.size());
}

bool is_cpp_editor_payload(std::string_view buffer, std::size_t cursor_byte) {
  if (cursor_byte > buffer.size()) {
    return false;
  }
  const Payload payload = cpp_payload(buffer, cursor_byte);
  return payload.valid;
}

bool editor_wants_more_input(std::string_view buffer, std::size_t cursor_byte) {
  if (cursor_byte > buffer.size()) {
    return false;
  }
  const Payload payload = cpp_payload(buffer, cursor_byte);
  return payload.valid && incomplete_at_cursor(payload.code, payload.cursor);
}

DelimiterDecision delimiter_decision(std::string_view buffer, std::size_t cursor_byte,
                                     char32_t key) {
  if (cursor_byte > buffer.size() || !is_cpp_editor_payload(buffer, cursor_byte)) {
    return {};
  }
  const Payload payload = cpp_payload(buffer, cursor_byte);
  const auto header_context = include_context(payload.code, payload.cursor);
  const char closing = matching_close(key);
  if (key == '<') {
    return include_context(payload.code, payload.cursor) == IncludeContext::BeforeHeader
               ? DelimiterDecision{DelimiterAction::InsertPair, '>'}
               : DelimiterDecision{};
  }
  if (key == '>') {
    return payload.cursor < payload.code.size() && payload.code[payload.cursor] == '>' &&
                   header_context == IncludeContext::InsideAngleHeader
               ? DelimiterDecision{DelimiterAction::SkipClosing, 0}
               : DelimiterDecision{};
  }
  if (key == '\b' || key == 127) {
    if (cursor_byte == 0 || cursor_byte == buffer.size()) {
      return {};
    }
    const char open = buffer[cursor_byte - 1];
    if (open == '<' && buffer[cursor_byte] == '>' && payload.cursor > 0 &&
        include_context(payload.code, payload.cursor - 1) == IncludeContext::BeforeHeader) {
      return {DelimiterAction::DeletePair, 0};
    }
    const char close = matching_close(static_cast<unsigned char>(open));
    if (close == 0 || buffer[cursor_byte] != close) {
      return {};
    }

    const auto lexed = lex_cpp(payload.code);
    if (open == '"' || open == '\'') {
      for (const auto& token : lexed.tokens) {
        if (token.begin == payload.cursor - 1 && token.end == payload.cursor + 1 &&
            ((open == '"' && token.kind == TokenKind::String) ||
             (open == '\'' && token.kind == TokenKind::Char))) {
          return {DelimiterAction::DeletePair, 0};
        }
        if (open == '"' && token.kind == TokenKind::Preprocessor &&
            token.begin <= payload.cursor - 1 && token.end >= payload.cursor + 1 &&
            payload.code[payload.cursor - 1] == '"' && payload.code[payload.cursor] == '"') {
          return {DelimiterAction::DeletePair, 0};
        }
      }
    } else {
      bool opening = false;
      bool matching = false;
      for (const auto& token : lexed.tokens) {
        if (token.begin == payload.cursor - 1 && token.kind == TokenKind::Bracket &&
            payload.code[token.begin] == open) {
          opening = true;
        }
        if (token.begin == payload.cursor && token.kind == TokenKind::Bracket &&
            payload.code[token.begin] == close) {
          matching = true;
        }
      }
      if (opening && matching) {
        return {DelimiterAction::DeletePair, 0};
      }
    }
    return {};
  }

  // Header names are preprocessing tokens. Ordinary delimiter keys are
  // filename characters here, so leave insertion and closing keys untouched.
  if (header_context == IncludeContext::InsideAngleHeader) {
    return {};
  }

  if (key > 0x7f) {
    return {};
  }
  const bool quote = key == '"' || key == '\'';
  if (key == ')' || key == ']' || key == '}') {
    if (cursor_byte < buffer.size() && buffer[cursor_byte] == static_cast<char>(key) && !quote) {
      const auto lexed = lex_cpp(payload.code);
      for (const auto& token : lexed.tokens) {
        if (token.begin == payload.cursor &&
            (token.kind == TokenKind::Bracket || token.kind == TokenKind::UnmatchedBracket) &&
            payload.code[token.begin] == static_cast<char>(key)) {
          return {DelimiterAction::SkipClosing, 0};
        }
      }
    }
    return {};
  }

  if (closing == 0) {
    return {};
  }

  if (quote && cursor_byte < buffer.size() && buffer[cursor_byte] == static_cast<char>(key) &&
      !escaped_at(payload.code, payload.cursor)) {
    const auto lexed = lex_cpp(payload.code);
    for (const auto& token : lexed.tokens) {
      if (token.begin <= payload.cursor && payload.cursor < token.end &&
          (token.kind == TokenKind::String || token.kind == TokenKind::Char) &&
          payload.code[payload.cursor] == static_cast<char>(key)) {
        // Escapes split a literal into several lexer tokens, so the current
        // token need not contain its opening quote.
        if (token.end == payload.cursor + 1 &&
            in_literal_or_comment(payload.code, payload.cursor)) {
          return {DelimiterAction::SkipClosing, 0};
        }
      }
      if (key == '"' && token.kind == TokenKind::Preprocessor && token.begin <= payload.cursor &&
          token.end > payload.cursor && in_preprocessor_quote(payload.code, payload.cursor)) {
        return {DelimiterAction::SkipClosing, 0};
      }
    }
  }

  if (in_literal_or_comment(payload.code, payload.cursor)) {
    return {};
  }
  if (key == '"' && raw_string_prefix_before(payload.code, payload.cursor)) {
    return {};
  }
  if (key == '\'' && number_ends_at(payload.code, payload.cursor)) {
    return {};
  }
  if (quote && escaped_at(payload.code, payload.cursor)) {
    return {};
  }
  return {DelimiterAction::InsertPair, static_cast<unsigned char>(closing)};
}

} // namespace example
