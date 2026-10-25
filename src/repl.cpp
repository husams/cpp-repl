#include "repl.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "highlight_repl.hpp"
#include "runtime_inspection.hpp"
#include "session_commands.hpp"

namespace example {

namespace {

using replxx::Replxx;

bool starts_with(const std::string& s, std::string_view prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

// Start of the path being typed at the end of a %lib / %add-path / %add line:
// after the last blank, or for %add-path also after the last ':'.
std::size_t current_path_begin(const std::string& input, const ParsedCommand& cmd) {
  const char* stops = cmd.kind == CommandKind::AddPath ? ": \t" : " \t";
  const std::size_t pos = input.find_last_of(stops);
  return pos == std::string::npos || pos < cmd.arg_begin ? cmd.arg_begin : pos + 1;
}

// Tab: C++ is completed by the interpreter; `%` + Tab completes command
// names, Tab after "%lib " / "%add-path " / "%add " the filesystem path being
// typed and after "%class " the session's class names.
Replxx::completions_t complete(CppInterpreter& cpp, const std::string& input,
                               int& context_len) {
  Replxx::completions_t out;
  if (!is_command_line(input)) {
    const CppInterpreter::Completions c = cpp.complete(input);
    context_len = static_cast<int>(utf8_length(c.prefix));
    for (const std::string& name : c.names) out.emplace_back(name);
    return out;
  }
  const ParsedCommand cmd = parse_command(input);

  if (cmd.code && cmd.incomplete) {
    const auto completion = cpp.complete(cmd.code->value);
    context_len = static_cast<int>(utf8_length(completion.prefix));
    for (const auto& name : completion.names) out.emplace_back(name);
    return out;
  }

  if (cmd.name_end == input.size()) {  // still typing the command word
    const std::string token = input.substr(cmd.name_begin);
    context_len = static_cast<int>(utf8_length(token));
    for (const std::string& name : command_names()) {
      if (starts_with(name, token)) out.emplace_back(name);
    }
    return out;
  }

  if (cmd.kind == CommandKind::Lib || cmd.kind == CommandKind::AddPath ||
      cmd.kind == CommandKind::Add) {
    const std::string typed = input.substr(current_path_begin(input, cmd));
    context_len = static_cast<int>(utf8_length(typed));
    for (const std::string& p :
         complete_path(typed, cmd.kind == CommandKind::AddPath)) {
      out.emplace_back(p);
    }
  } else if (cmd.kind == CommandKind::CompileOpts ||
             cmd.kind == CommandKind::AddCompileOpt ||
             cmd.kind == CommandKind::SetCompileOpts ||
             cmd.kind == CommandKind::UseCompileOpts) {
    // Only the first operand can be a filename. Flag values may themselves be
    // paths; do not mistake those for the scope operand.
    auto typed = input.substr(cmd.arg_begin);
    if (!typed.empty() && typed.front() == '-') return out;
    char quote = 0;
    if (!typed.empty() && (typed.front() == '"' || typed.front() == '\'')) {
      quote = typed.front();
      typed.erase(0, 1);
      const auto close = typed.find(quote);
      if (close != std::string::npos) {
        if (close + 1 != typed.size()) return out;
        typed.pop_back();
      }
    } else if (typed.find_first_of(" \t") != std::string::npos)
      return out;
    context_len = static_cast<int>(utf8_length(typed));
    for (auto path : complete_path(typed, false)) {
      if (quote && !path.empty() && path.back() != '/') path += quote;
      out.emplace_back(path);
    }
  } else if (cmd.kind == CommandKind::Libs && !cmd.all_libraries) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    if (starts_with("all", typed)) out.emplace_back("all");
  } else if (cmd.kind == CommandKind::LibInfo) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    for (const auto& library : loaded_libraries())
      if (starts_with(library.name, typed)) out.emplace_back(library.name);
  } else if (cmd.kind == CommandKind::Class || cmd.kind == CommandKind::Info) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    for (const std::string& name : cmd.kind == CommandKind::Class ? cpp.class_names() : cpp.symbol_names()) {
      if (starts_with(name, typed)) out.emplace_back(name);
    }
  } else if (cmd.kind == CommandKind::Symbols) {
    // A regex can itself start with "lib" or "name"; complete only keywords.
    for (const auto* filter : {&cmd.library_regex, &cmd.name_regex}) {
      if (*filter && (*filter)->span.end == input.size()) return out;
    }
    auto last_filter = cmd.library_regex && cmd.name_regex ?
        std::max(cmd.library_regex->span.end, cmd.name_regex->span.end) :
        cmd.library_regex ? cmd.library_regex->span.end :
        cmd.name_regex ? cmd.name_regex->span.end : cmd.name_end;
    if (cmd.all_symbols) last_filter = std::max(last_filter, cmd.all_symbols->end);
    const auto remaining = input.find_first_not_of(" \t", last_filter);
    if (remaining != std::string::npos) {
      const auto word_end = input.find_first_of(" \t", remaining);
      // After a complete keyword and a blank the user is entering its regex.
      if (word_end != std::string::npos) return out;
    }
    const auto begin = input.find_last_of(" \t");
    const auto typed = input.substr(begin + 1);
    context_len = static_cast<int>(utf8_length(typed));
    if (!cmd.library_regex && starts_with("lib", typed)) out.emplace_back("lib");
    if (!cmd.name_regex && starts_with("name", typed)) out.emplace_back("name");
    if (!cmd.all_symbols && starts_with("all", typed)) out.emplace_back("all");
  }
  return out;
}

Replxx::hints_t hint(const std::string& input, int& context_len,
                     Replxx::Color& color) {
  Replxx::hints_t out;
  if (!is_command_line(input)) return out;
  const ParsedCommand cmd = parse_command(input);
  color = Replxx::Color::GRAY;

  if (cmd.name_end == input.size()) {  // matching command names
    const std::string token = input.substr(cmd.name_begin);
    context_len = static_cast<int>(utf8_length(token));
    for (const std::string& name : command_names()) {
      if (starts_with(name, token)) out.emplace_back(name);
    }
  } else if (cmd.arg_begin == input.size()) {  // "<cmd> " -> show usage
    const CommandInfo* info = find_command(cmd.name);
    if (info != nullptr && info->usage[0] != '\0') {
      context_len = 0;
      out.emplace_back(info->usage + 1);
    }
  }
  return out;
}

// Enter: submit commands and complete C++; otherwise keep editing in the same
// buffer, including blank lines. Ctrl+C cancels an incomplete block.
Replxx::ACTION_RESULT on_enter(Replxx& rx, char32_t code) {
  const std::string text = rx.get_state().text();
  if (!wants_more_input(text)) return rx.invoke(Replxx::ACTION::COMMIT_LINE, code);
  return rx.invoke(Replxx::ACTION::NEW_LINE, code);
}

}  // namespace

void configure(Replxx& rx, CppInterpreter& cpp) {
  rx.set_max_history_size(1000);
  rx.set_unique_history(true);
  rx.set_word_break_characters(" \t\n");
  rx.set_completion_callback([&cpp](const std::string& input, int& context_len) {
    return complete(cpp, input, context_len);
  });
  rx.set_hint_callback(hint);
  rx.set_highlighter_callback(highlight_input);
  rx.set_max_hint_rows(4);

  // Custom binding: Ctrl+L clears the screen and redraws the prompt.
  rx.bind_key(Replxx::KEY::control('L'), [&rx](char32_t code) {
    return rx.invoke(Replxx::ACTION::CLEAR_SCREEN, code);
  });

  // Custom binding: Enter inserts a newline while the C++ input is incomplete.
  rx.bind_key(Replxx::KEY::ENTER,
              [&rx](char32_t code) { return on_enter(rx, code); });
}

bool execute(LibraryManager& libs, CppInterpreter& cpp, const std::string& line) {
  const auto cmd = parse_command(line);
  if (cmd.kind == CommandKind::Cpp) {
    cpp.execute(line);
    return true;
  }
  return execute_command(libs, cpp, cmd, std::cout).keep_running;
}

}  // namespace example
