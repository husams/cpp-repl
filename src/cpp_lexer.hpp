#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace example {

enum class TokenKind {
  Keyword,
  Type,  // built-in and common std types
  Identifier,
  String,  // string literal text (including prefix and quotes), minus escapes
  Char,    // char literal text, minus escapes
  Escape,  // escape sequence inside a string or char literal
  Number,
  Comment,
  Preprocessor,
  Operator,
  Bracket,           // ( ) [ ] { } that is matched or still open
  UnmatchedBracket,  // closing bracket with no (matching) opener
  Other,
};

// Half-open byte range [begin, end) into the lexed source. Whitespace is not
// tokenized. Tokens never overlap and are in source order.
struct Token {
  TokenKind kind;
  std::size_t begin;
  std::size_t end;
  int depth = 0;  // nesting depth for Bracket tokens (outermost pair is 0)
};

struct LexResult {
  std::vector<Token> tokens;
  std::string open_brackets;  // still-open "([{" at end of input, outermost first
  // The input ends inside a string, char or raw string literal, a block
  // comment, or a preprocessor line continued with a trailing backslash.
  bool unterminated = false;
};

// Lexes (possibly multi-line, possibly incomplete) C++ source. Never fails.
LexResult lex_cpp(std::string_view src);

// True if a REPL should keep reading instead of submitting `src`: unbalanced
// brackets, unterminated literal/comment, or a trailing operator or comma.
bool is_incomplete(std::string_view src);

// True if `src` has a `return` outside any braces (e.g. "return 1;" or
// "if (x) return;"), which crashes clang::Interpreter 22.
bool has_top_level_return(std::string_view src);

// The identifier being typed at the end of `text` ("" after a blank, '.' etc).
std::string_view trailing_identifier(std::string_view text);

// A member access being typed at the end of the text, e.g. "a.b->na":
// names {"a", "b"}, arrows {false, true} (arrows[i]: the access after
// names[i] is "->"), member_prefix "na".
struct MemberAccess {
  std::vector<std::string> names;
  std::vector<bool> arrows;
  std::string member_prefix;
};

// Recognizes `ident { ('.' | '->') ident } ('.' | '->') [ident]` at the end
// of `text`; nullopt for anything else (e.g. "f().", "3.", "ns::x.").
std::optional<MemberAccess> trailing_member_access(std::string_view text);

}  // namespace example
