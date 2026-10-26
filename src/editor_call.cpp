#include "editor_call.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp_lexer.hpp"

namespace example {
namespace {

struct LexToken {
  const Token* token;
  std::string_view text;
};

bool is_hidden(TokenKind kind) {
  return kind == TokenKind::String || kind == TokenKind::Char || kind == TokenKind::Escape ||
         kind == TokenKind::Comment || kind == TokenKind::Preprocessor;
}

bool is_open(std::string_view text) {
  return text == "(" || text == "[" || text == "{";
}

bool is_close(std::string_view text) {
  return text == ")" || text == "]" || text == "}";
}

bool closes(std::string_view open, std::string_view close) {
  return (open == "(" && close == ")") || (open == "[" && close == "]") ||
         (open == "{" && close == "}");
}

bool excluded_function_keyword(std::string_view word) {
  return word == "if" || word == "for" || word == "while" || word == "switch" || word == "catch" ||
         word == "sizeof" || word == "alignof" || word == "decltype" || word == "noexcept" ||
         word == "static_cast" || word == "dynamic_cast" || word == "reinterpret_cast" ||
         word == "const_cast";
}

std::size_t previous_code(const std::vector<LexToken>& tokens, std::size_t before) {
  while (before > 0) {
    --before;
    if (!is_hidden(tokens[before].token->kind)) {
      return before;
    }
  }
  return tokens.size();
}

std::size_t next_code(const std::vector<LexToken>& tokens, std::size_t after) {
  for (++after; after < tokens.size(); ++after) {
    if (!is_hidden(tokens[after].token->kind)) {
      return after;
    }
  }
  return tokens.size();
}

bool can_end_callable(const LexToken& token) {
  const TokenKind kind = token.token->kind;
  if (kind == TokenKind::Identifier || kind == TokenKind::Type) {
    return true;
  }
  if (kind == TokenKind::Keyword) {
    return !excluded_function_keyword(token.text);
  }
  if (token.text == ")" || token.text == "]" || token.text == "}" || token.text == ">") {
    return true;
  }
  // Lambdas and function objects can be followed by a call parenthesis.
  return token.text == "++" || token.text == "--";
}

bool is_call_parenthesis(const std::vector<LexToken>& tokens, std::size_t open_index) {
  const std::size_t previous = previous_code(tokens, open_index);
  if (previous == tokens.size()) {
    return false;
  }
  const LexToken& token = tokens[previous];
  if (token.token->kind == TokenKind::String || token.token->kind == TokenKind::Char) {
    return true;
  }
  return can_end_callable(token);
}

// Returns the index of the '>' or '>>' token which closes a plausible
// template-id beginning at '<'. Comparisons such as `a < b, c` have no close,
// while `a < b > c, d` is rejected by the following-token check.
std::size_t template_close(const std::vector<LexToken>& tokens, std::size_t less_index,
                           std::size_t limit) {
  const std::size_t previous = previous_code(tokens, less_index);
  if (previous == tokens.size() || !can_end_callable(tokens[previous])) {
    return tokens.size();
  }
  const std::size_t first = next_code(tokens, less_index);
  if (first >= limit || tokens[first].text == "," || tokens[first].text == ">") {
    return tokens.size();
  }

  int angles = 1;
  std::vector<std::string_view> brackets;
  for (std::size_t i = first; i < limit; ++i) {
    const std::string_view text = tokens[i].text;
    if (is_open(text)) {
      brackets.push_back(text);
      continue;
    }
    if (is_close(text)) {
      if (!brackets.empty() && closes(brackets.back(), text)) {
        brackets.pop_back();
        continue;
      }
      return tokens.size();
    }
    if (!brackets.empty()) {
      continue;
    }

    if (text == "<") {
      ++angles;
    } else if (text == ">" || text == ">>") {
      angles -= text == ">>" ? 2 : 1;
      if (angles <= 0) {
        const std::size_t after = next_code(tokens, i);
        if (after == tokens.size() || after >= limit) {
          return i;
        }
        const std::string_view following = tokens[after].text;
        if (following == "(" || following == "{" || following == "::" || following == "." ||
            following == "->" || following == "," || following == ")" || following == "]" ||
            following == "}" || following == ";" || following == "(" || following == ">" ||
            following == "&&" || following == "&" || following == "*") {
          return i;
        }
        return tokens.size();
      }
    } else if ((text == "," || text == ";" || text == "?") && angles == 1) {
      // A top-level separator before the closer is evidence of a comparison.
      // Template argument lists themselves may contain commas, so retain the
      // candidate only when a matching closer is actually found.
    }
  }
  return tokens.size();
}

bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

EditorArgument trimmed_argument(std::string_view text, std::size_t begin, std::size_t end,
                                const std::vector<LexToken>& tokens) {
  bool changed = true;
  while (changed) {
    changed = false;
    while (begin < end && is_space(text[begin])) {
      ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
      --end;
    }
    for (const LexToken& token : tokens) {
      if (token.token->kind != TokenKind::Comment) {
        continue;
      }
      if (token.token->begin == begin && token.token->end <= end) {
        begin = token.token->end;
        changed = true;
        break;
      }
      if (token.token->end == end && token.token->begin >= begin) {
        end = token.token->begin;
        changed = true;
        break;
      }
    }
  }
  return {begin, end};
}

} // namespace

std::optional<EditorCall> editor_call_at_cursor(std::string_view text, std::size_t cursor_byte) {
  if (cursor_byte > text.size()) {
    return std::nullopt;
  }

  const LexResult lexed = lex_cpp(text);
  std::vector<LexToken> tokens;
  tokens.reserve(lexed.tokens.size());
  for (const Token& token : lexed.tokens) {
    const std::string_view token_text = text.substr(token.begin, token.end - token.begin);
    if (is_hidden(token.kind) && token.begin <= cursor_byte && cursor_byte < token.end) {
      return std::nullopt;
    }
    tokens.push_back({&token, token_text});
  }

  std::vector<std::size_t> matching(tokens.size(), tokens.size());
  std::vector<std::size_t> stack;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const std::string_view current = tokens[i].text;
    if (is_open(current)) {
      stack.push_back(i);
    } else if (is_close(current) && !stack.empty() && closes(tokens[stack.back()].text, current)) {
      matching[stack.back()] = i;
      matching[i] = stack.back();
      stack.pop_back();
    }
  }

  std::size_t selected = tokens.size();
  std::size_t selected_close = tokens.size();
  std::size_t latest_open = 0;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].text != "(" || !is_call_parenthesis(tokens, i)) {
      continue;
    }
    const std::size_t after_open = tokens[i].token->end;
    const std::size_t close_index = matching[i];
    const std::size_t close_byte =
        close_index == tokens.size() ? text.size() : tokens[close_index].token->begin;
    if (cursor_byte < after_open || cursor_byte > close_byte) {
      continue;
    }
    if (selected == tokens.size() || tokens[i].token->begin >= latest_open) {
      selected = i;
      selected_close = close_index;
      latest_open = tokens[i].token->begin;
    }
  }
  if (selected == tokens.size()) {
    return std::nullopt;
  }

  const std::size_t content_begin = tokens[selected].token->end;
  const bool has_close = selected_close != tokens.size();
  const std::size_t content_end = has_close ? tokens[selected_close].token->begin : text.size();
  std::vector<std::pair<std::size_t, std::size_t>> slots;
  std::vector<std::string_view> nested;
  std::size_t slot_begin = content_begin;
  std::size_t end_token = has_close ? selected_close : tokens.size();

  for (std::size_t i = selected + 1; i < end_token; ++i) {
    if (is_hidden(tokens[i].token->kind)) {
      continue;
    }
    const std::string_view token = tokens[i].text;
    if (is_open(token)) {
      nested.push_back(token);
      continue;
    }
    if (is_close(token)) {
      if (!nested.empty() && closes(nested.back(), token)) {
        nested.pop_back();
      }
      continue;
    }
    if (!nested.empty()) {
      continue;
    }

    if (token == "<") {
      const std::size_t close = template_close(tokens, i, end_token);
      if (close != tokens.size()) {
        i = close;
        continue;
      }
    }
    if (token == ",") {
      const std::size_t comma = tokens[i].token->begin;
      slots.emplace_back(slot_begin, comma);
      slot_begin = tokens[i].token->end;
    }
  }
  slots.emplace_back(slot_begin, content_end);

  // `f()` is one empty UI slot, just like `f(,)` has two empty slots.
  if (slots.size() == 1) {
    const bool has_payload_token = std::any_of(
        tokens.begin() + static_cast<std::ptrdiff_t>(selected + 1),
        tokens.begin() + static_cast<std::ptrdiff_t>(end_token), [](const LexToken& token) {
          return token.token->kind != TokenKind::Comment &&
                 token.token->kind != TokenKind::Preprocessor &&
                 token.token->kind != TokenKind::Escape;
        });
    if (!has_payload_token) {
      slots[0] = {content_begin, content_begin};
    }
  }

  EditorCall result{tokens[selected].token->begin,
                    has_close ? tokens[selected_close].token->begin : text.size(),
                    has_close,
                    {},
                    0};
  result.arguments.reserve(slots.size());
  for (const auto& slot : slots) {
    result.arguments.push_back(trimmed_argument(text, slot.first, slot.second, tokens));
  }
  for (std::size_t i = 0; i + 1 < slots.size(); ++i) {
    if (cursor_byte >= slots[i].second) {
      result.active_argument = static_cast<unsigned>(i + 1);
    }
  }
  result.active_argument =
      std::min(result.active_argument, static_cast<unsigned>(result.arguments.size() - 1));
  return result;
}

} // namespace example
