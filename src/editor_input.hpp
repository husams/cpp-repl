#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace example {

enum class DelimiterAction { None, InsertPair, SkipClosing, DeletePair };

struct DelimiterDecision {
  DelimiterAction action = DelimiterAction::None;
  char32_t closing = 0;
};

// Decide how an explicitly typed editor key should affect a C++ input buffer.
// cursor_byte is a UTF-8 byte offset into the complete buffer.
DelimiterDecision delimiter_decision(std::string_view buffer, std::size_t cursor_byte,
                                     char32_t key);

// Convert a Unicode code-point offset in a UTF-8 string to a byte offset.
std::size_t utf8_byte_offset(std::string_view text, std::size_t codepoint_offset);

// Return true when the cursor is in C++ payload, including the C++ body of a
// command such as %mock, rather than in command syntax or a shell/path operand.
bool is_cpp_editor_payload(std::string_view buffer, std::size_t cursor_byte);

} // namespace example
