#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace example {

// Half-open UTF-8 byte offsets into the complete editor buffer. Whitespace at
// either edge is excluded; an empty argument has begin == end.
struct EditorArgument {
  std::size_t begin;
  std::size_t end;
};

struct EditorCall {
  // Byte offset of the opening parenthesis.
  std::size_t open;
  // Byte offset of the closing parenthesis, or text.size() if it is missing.
  std::size_t close;
  bool has_close;
  // A syntactically empty call still has one empty slot for the editor UI.
  std::vector<EditorArgument> arguments;
  unsigned active_argument;
};

// Finds the innermost lexically recognizable call containing cursor_byte.
// Returns nullopt when the cursor is in a string, character literal, comment,
// preprocessor directive, or outside any call. This is a fast lexical model;
// callers should use semantic metadata to decide whether a call is actionable.
std::optional<EditorCall> editor_call_at_cursor(std::string_view text, std::size_t cursor_byte);

} // namespace example
