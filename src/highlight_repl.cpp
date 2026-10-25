#include "highlight_repl.hpp"

#include <algorithm>
#include <string_view>

#include "commands.hpp"
#include "highlight_cpp.hpp"

namespace example {

using replxx::Replxx;

namespace {

// Colors a byte range of `input` (replxx indexes by code point).
void paint(const std::string& input, Span span, Replxx::Color color,
           Replxx::colors_t& colors) {
  const std::string_view text(input);
  const std::size_t first = utf8_length(text.substr(0, span.begin));
  const std::size_t last =
      first + utf8_length(text.substr(span.begin, span.end - span.begin));
  for (std::size_t i = first; i < std::min(last, colors.size()); ++i) {
    colors[i] = color;
  }
}

}  // namespace

void highlight_input(const std::string& input, Replxx::colors_t& colors) {
  if (!is_command_line(input)) {
    highlight_cpp(input, colors);
    return;
  }
  const ParsedCommand cmd = parse_command(input);
  paint(input, {cmd.name_begin, cmd.name_end}, kCommandColor, colors);
  for (const Operand& p : cmd.paths) paint(input, p.span, kPathColor, colors);
  for (const Operand& flag : cmd.compile_flags)
    paint(input, flag.span, kClassNameColor, colors);
  for (const Span& s : cmd.separators) paint(input, s, kSeparatorColor, colors);
  if (cmd.class_name) paint(input, cmd.class_name->span, kClassNameColor, colors);
  if (cmd.symbol) paint(input, cmd.symbol->span, kClassNameColor, colors);
  if (cmd.library_regex) paint(input, cmd.library_regex->span, kPathColor, colors);
  if (cmd.name_regex) paint(input, cmd.name_regex->span, kClassNameColor, colors);
  for (const auto* operand : {&cmd.signature, &cmd.caller, &cmd.code}) {
    if (!*operand) continue;
    Replxx::colors_t part(utf8_length((*operand)->value), Replxx::Color::DEFAULT);
    highlight_cpp((*operand)->value, part);
    const auto offset = utf8_length(std::string_view(input).substr(0, (*operand)->span.begin));
    for (std::size_t i = 0; i < part.size() && offset + i < colors.size(); ++i)
      colors[offset + i] = part[i];
  }
  if (!cmd.error.empty() && !cmd.incomplete) paint(input, cmd.error_span, kCommandErrorColor, colors);
}

}  // namespace example
