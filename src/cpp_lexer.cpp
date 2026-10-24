#include "cpp_lexer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace example {

namespace {

const std::unordered_set<std::string_view>& keywords() {
  static const std::unordered_set<std::string_view> kSet = {
      "alignas",      "alignof",     "and",        "and_eq",
      "asm",          "auto",        "bitand",     "bitor",
      "break",        "case",        "catch",      "class",
      "compl",        "const",       "const_cast", "constexpr",
      "continue",     "decltype",    "default",    "delete",
      "do",           "dynamic_cast", "else",      "enum",
      "explicit",     "export",      "extern",     "false",
      "final",        "for",         "friend",     "goto",
      "if",           "inline",      "mutable",    "namespace",
      "new",          "noexcept",    "not",        "not_eq",
      "nullptr",      "operator",    "or",         "or_eq",
      "override",     "private",     "protected",  "public",
      "register",     "reinterpret_cast", "return", "sizeof",
      "static",       "static_assert", "static_cast", "struct",
      "switch",       "template",    "this",       "thread_local",
      "throw",        "true",        "try",        "typedef",
      "typeid",       "typename",    "union",      "using",
      "virtual",      "volatile",    "while",      "xor",
      "xor_eq"};
  return kSet;
}

const std::unordered_set<std::string_view>& types() {
  static const std::unordered_set<std::string_view> kSet = {
      // built-in
      "void", "bool", "char", "wchar_t", "char8_t", "char16_t", "char32_t",
      "short", "int", "long", "float", "double", "signed", "unsigned",
      // <cstddef>, <cstdint>
      "size_t", "ssize_t", "ptrdiff_t", "nullptr_t", "intptr_t", "uintptr_t",
      "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
      "uint32_t", "uint64_t",
      // common std
      "string", "string_view", "wstring", "vector", "array", "deque", "list",
      "forward_list", "map", "multimap", "unordered_map", "set", "multiset",
      "unordered_set", "stack", "queue", "priority_queue", "pair", "tuple",
      "optional", "variant", "any", "span", "function", "unique_ptr",
      "shared_ptr", "weak_ptr", "istream", "ostream", "iostream",
      "stringstream", "ifstream", "ofstream", "thread", "mutex", "atomic"};
  return kSet;
}

bool is_ident_start(unsigned char c) {
  return std::isalpha(c) != 0 || c == '_' || c >= 0x80;
}

bool is_ident_char(unsigned char c) {
  return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

bool is_raw_prefix(std::string_view w) {
  return w == "R" || w == "u8R" || w == "uR" || w == "UR" || w == "LR";
}

bool is_string_prefix(std::string_view w) {
  return w == "u8" || w == "u" || w == "U" || w == "L";
}

char closer_for(char open) {
  return open == '(' ? ')' : (open == '[' ? ']' : '}');
}

class Lexer {
 public:
  explicit Lexer(std::string_view s) : s_(s) {}

  LexResult run() {
    bool line_start = true;
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '\n') {
        line_start = true;
        ++i_;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        ++i_;
        continue;
      }
      const bool at_line_start = line_start;
      line_start = false;

      if (c == '#' && at_line_start) {
        preprocessor();
      } else if (c == '/' && peek(1) == '/') {
        line_comment();
      } else if (c == '/' && peek(1) == '*') {
        block_comment();
      } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                 (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
        number();
      } else if (is_ident_start(static_cast<unsigned char>(c))) {
        identifier_or_literal();
      } else if (c == '"' || c == '\'') {
        quoted(i_, c);
      } else if (c == '(' || c == '[' || c == '{' || c == ')' || c == ']' ||
                 c == '}') {
        bracket(c);
      } else {
        operator_or_other();
      }
    }
    result_.open_brackets = stack_;
    return std::move(result_);
  }

 private:
  char peek(std::size_t off = 0) const {
    return i_ + off < s_.size() ? s_[i_ + off] : '\0';
  }

  void emit(TokenKind kind, std::size_t begin, std::size_t end, int depth = 0) {
    result_.tokens.push_back({kind, begin, end, depth});
  }

  void preprocessor() {
    const std::size_t begin = i_;
    std::size_t j = i_;
    while (j < s_.size()) {
      if (s_[j] == '\\' && j + 1 < s_.size() && s_[j + 1] == '\n') {
        j += 2;
      } else if (s_[j] == '\n') {
        break;
      } else {
        ++j;
      }
    }
    if (j == s_.size() && (s_.back() == '\\' ||
                           (s_.size() >= 2 && s_.back() == '\n' &&
                            s_[s_.size() - 2] == '\\'))) {
      result_.unterminated = true;
    }
    i_ = j;
    emit(TokenKind::Preprocessor, begin, j);
  }

  void line_comment() {
    const std::size_t begin = i_;
    while (i_ < s_.size() && s_[i_] != '\n') ++i_;
    emit(TokenKind::Comment, begin, i_);
  }

  void block_comment() {
    const std::size_t begin = i_;
    const std::size_t close = s_.find("*/", i_ + 2);
    if (close == std::string_view::npos) {
      i_ = s_.size();
      result_.unterminated = true;
    } else {
      i_ = close + 2;
    }
    emit(TokenKind::Comment, begin, i_);
  }

  void number() {
    const std::size_t begin = i_;
    bool hex = false;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
      hex = true;
      i_ += 2;
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
      i_ += 2;
    }
    while (i_ < s_.size()) {
      const char d = s_[i_];
      if (std::isalnum(static_cast<unsigned char>(d)) != 0 || d == '_' ||
          d == '.') {
        const bool exponent =
            hex ? (d == 'p' || d == 'P') : (d == 'e' || d == 'E');
        ++i_;
        if (exponent && (peek() == '+' || peek() == '-')) ++i_;
      } else if (d == '\'' &&
                 std::isalnum(static_cast<unsigned char>(peek(1))) != 0) {
        ++i_;  // digit separator
      } else {
        break;
      }
    }
    emit(TokenKind::Number, begin, i_);
  }

  void identifier_or_literal() {
    const std::size_t begin = i_;
    while (i_ < s_.size() && is_ident_char(static_cast<unsigned char>(s_[i_]))) {
      ++i_;
    }
    const std::string_view word = s_.substr(begin, i_ - begin);

    if (peek() == '"' && is_raw_prefix(word) && raw_string(begin)) return;
    if (peek() == '"' && is_string_prefix(word)) {
      quoted(begin, '"');
      return;
    }
    if (peek() == '\'' && is_string_prefix(word)) {
      quoted(begin, '\'');
      return;
    }

    TokenKind kind = TokenKind::Identifier;
    if (keywords().count(word) != 0) {
      kind = TokenKind::Keyword;
    } else if (types().count(word) != 0) {
      kind = TokenKind::Type;
    }
    emit(kind, begin, i_);
  }

  // i_ is at the opening '"' of R"delim( ... )delim". Returns false (without
  // consuming anything) when the delimiter is malformed, so the caller can fall
  // back to lexing an identifier followed by an ordinary string.
  bool raw_string(std::size_t begin) {
    std::size_t j = i_ + 1;
    const std::size_t delim_begin = j;
    while (j < s_.size() && s_[j] != '(') {
      const char d = s_[j];
      if (d == ')' || d == '\\' || d == '"' || d == '\n' ||
          std::isspace(static_cast<unsigned char>(d)) != 0 ||
          j - delim_begin >= 16) {
        return false;
      }
      ++j;
    }
    if (j >= s_.size()) {  // still typing the delimiter
      i_ = s_.size();
      result_.unterminated = true;
      emit(TokenKind::String, begin, i_);
      return true;
    }
    const std::string closing =
        ")" + std::string(s_.substr(delim_begin, j - delim_begin)) + "\"";
    const std::size_t close = s_.find(closing, j + 1);
    if (close == std::string_view::npos) {
      i_ = s_.size();
      result_.unterminated = true;
    } else {
      i_ = close + closing.size();
    }
    emit(TokenKind::String, begin, i_);
    return true;
  }

  // Ordinary string or char literal. `begin` includes any encoding prefix; i_
  // is at the opening quote. Escapes are emitted as separate tokens.
  void quoted(std::size_t begin, char quote) {
    const TokenKind kind = quote == '"' ? TokenKind::String : TokenKind::Char;
    std::size_t piece = begin;
    ++i_;
    auto flush = [&](std::size_t end) {
      if (end > piece) emit(kind, piece, end);
    };
    while (true) {
      if (i_ >= s_.size()) {
        result_.unterminated = true;
        flush(s_.size());
        return;
      }
      const char c = s_[i_];
      if (c == quote) {
        ++i_;
        flush(i_);
        return;
      }
      if (c != '\\') {
        ++i_;
        continue;
      }
      flush(i_);
      std::size_t j = i_ + 1;
      if (j >= s_.size()) {
        result_.unterminated = true;
      } else {
        const char e = s_[j];
        auto take_hex = [&](std::size_t max) {
          std::size_t n = 0;
          while (j < s_.size() && n < max && is_hex(s_[j])) {
            ++j;
            ++n;
          }
        };
        if (e == 'x') {
          ++j;
          take_hex(static_cast<std::size_t>(-1));
        } else if (e == 'u') {
          ++j;
          take_hex(4);
        } else if (e == 'U') {
          ++j;
          take_hex(8);
        } else if (e >= '0' && e <= '7') {
          std::size_t n = 0;
          while (j < s_.size() && n < 3 && s_[j] >= '0' && s_[j] <= '7') {
            ++j;
            ++n;
          }
        } else {
          ++j;
        }
      }
      emit(TokenKind::Escape, i_, j);
      piece = j;
      i_ = j;
    }
  }

  void bracket(char c) {
    const std::size_t begin = i_++;
    if (c == '(' || c == '[' || c == '{') {
      emit(TokenKind::Bracket, begin, i_, static_cast<int>(stack_.size()));
      stack_.push_back(c);
    } else if (!stack_.empty() && closer_for(stack_.back()) == c) {
      stack_.pop_back();
      emit(TokenKind::Bracket, begin, i_, static_cast<int>(stack_.size()));
    } else {
      emit(TokenKind::UnmatchedBracket, begin, i_);
    }
  }

  void operator_or_other() {
    static constexpr std::array<std::string_view, 5> kThree = {
        "<<=", ">>=", "<=>", "->*", "..."};
    static constexpr std::array<std::string_view, 22> kTwo = {
        "::", "->", ".*", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
        "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##"};
    static constexpr std::string_view kSingle = "+-*/%&|^~!=<>?:;,.#";

    const std::size_t begin = i_;
    const std::string_view rest = s_.substr(i_);
    auto starts = [&](std::string_view op) { return rest.substr(0, op.size()) == op; };

    std::size_t len = 0;
    for (auto op : kThree) {
      if (starts(op)) len = 3;
    }
    if (len == 0) {
      for (auto op : kTwo) {
        if (starts(op)) len = 2;
      }
    }
    if (len == 0 && kSingle.find(s_[i_]) != std::string_view::npos) len = 1;

    if (len == 0) {
      ++i_;
      emit(TokenKind::Other, begin, i_);
    } else {
      i_ += len;
      emit(TokenKind::Operator, begin, i_);
    }
  }

  std::string_view s_;
  std::size_t i_ = 0;
  std::string stack_;
  LexResult result_;
};

// Operators after which an expression obviously continues on the next line.
bool continues_expression(std::string_view op) {
  static constexpr std::array<std::string_view, 29> kOps = {
      ",",  "=",  "+",  "-",  "*",  "/",  "%",  "&",  "|",  "^",
      "&&", "||", "<<", "?",  ".",  "->", "::", "+=", "-=", "*=",
      "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "==", "!="};
  return std::find(kOps.begin(), kOps.end(), op) != kOps.end() || op == "<=" ||
         op == ">=";
}

}  // namespace

LexResult lex_cpp(std::string_view src) { return Lexer(src).run(); }

bool is_incomplete(std::string_view src) {
  const LexResult r = lex_cpp(src);
  if (r.unterminated || !r.open_brackets.empty()) return true;
  for (auto it = r.tokens.rbegin(); it != r.tokens.rend(); ++it) {
    if (it->kind == TokenKind::Comment) continue;
    return it->kind == TokenKind::Operator &&
           continues_expression(src.substr(it->begin, it->end - it->begin));
  }
  return false;
}

bool has_top_level_return(std::string_view src) {
  int braces = 0;
  for (const Token& t : lex_cpp(src).tokens) {
    const std::string_view text = src.substr(t.begin, t.end - t.begin);
    if (t.kind == TokenKind::Bracket) {
      if (text == "{") ++braces;
      if (text == "}") --braces;
    } else if (t.kind == TokenKind::Keyword && text == "return" && braces <= 0) {
      return true;
    }
  }
  return false;
}

std::string_view trailing_identifier(std::string_view text) {
  std::size_t begin = text.size();
  while (begin > 0 && is_ident_char(static_cast<unsigned char>(text[begin - 1]))) --begin;
  return text.substr(begin);
}

std::optional<MemberAccess> trailing_member_access(std::string_view text) {
  MemberAccess access;
  access.member_prefix = std::string(trailing_identifier(text));
  if (!access.member_prefix.empty() &&
      !is_ident_start(static_cast<unsigned char>(access.member_prefix[0]))) {
    return std::nullopt;
  }
  std::size_t pos = text.size() - access.member_prefix.size();

  // Consumes a '.' or "->" ending at `pos`; false if there is none.
  auto take_access = [&](bool& arrow) {
    if (pos >= 1 && text[pos - 1] == '.') {
      arrow = false;
      pos -= 1;
      return true;
    }
    if (pos >= 2 && text.substr(pos - 2, 2) == "->") {
      arrow = true;
      pos -= 2;
      return true;
    }
    return false;
  };

  bool arrow = false;
  if (!take_access(arrow)) return std::nullopt;
  for (;;) {
    const std::string_view name = trailing_identifier(text.substr(0, pos));
    if (name.empty() || !is_ident_start(static_cast<unsigned char>(name[0]))) {
      return std::nullopt;
    }
    access.names.insert(access.names.begin(), std::string(name));
    access.arrows.insert(access.arrows.begin(), arrow);
    pos -= name.size();
    if (!take_access(arrow)) break;
  }
  if (pos >= 1 && text[pos - 1] == ':') return std::nullopt;  // qualified name
  return access;
}

}  // namespace example
