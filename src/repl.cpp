#include "repl.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "highlight_repl.hpp"
#include "inspection_output.hpp"
#include "runtime_inspection.hpp"

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

// %add [path ...]: adds each source file, or lists the added ones.
void add_sources(CppInterpreter& cpp, const ParsedCommand& cmd) {
  if (cmd.paths.empty()) {
    if (cpp.sources().empty()) std::cout << "no source files added\n";
    for (const std::string& file : cpp.sources()) std::cout << file << '\n';
    return;
  }
  for (const Operand& p : cmd.paths) {
    const CppInterpreter::AddResult r = cpp.add_source(p.value);
    std::cout << (r.ok ? "" : "error: ") << r.message << '\n';
  }
}

// %class [name]: lists the session's classes, or describes one.
void show_class(CppInterpreter& cpp, const ParsedCommand& cmd) {
  if (!cmd.class_name) {
    const std::vector<std::string> names = cpp.class_names();
    if (names.empty()) std::cout << "no classes defined\n";
    for (const std::string& name : names) std::cout << name << '\n';
    return;
  }
  std::string error;
  if (const auto synopsis = cpp.describe_class(cmd.class_name->value, error)) {
    std::cout << *synopsis;
  } else {
    std::cout << "error: " << error << '\n';
  }
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
  const ParsedCommand cmd = parse_command(line);
  if (cmd.kind == CommandKind::Cpp) {
    cpp.execute(line);  // errors are reported by the interpreter
    return true;
  }
  if (cmd.kind == CommandKind::Add && cmd.error.empty()) {
    add_sources(cpp, cmd);
    return true;
  }
  if (cmd.kind == CommandKind::Class && cmd.error.empty()) {
    show_class(cpp, cmd);
    return true;
  }
  if (cmd.kind == CommandKind::Info && cmd.error.empty()) {
    std::string error;
    if (const auto info = cpp.symbol_info(cmd.symbol->value, error)) std::cout << color_inspection(*info, inspection_colors_enabled());
    else std::cout << color_inspection("error: " + error + '\n', inspection_colors_enabled());
    return true;
  }
  if ((cmd.kind == CommandKind::Libs || cmd.kind == CommandKind::LibInfo) && cmd.error.empty()) {
    std::string error;
    const auto query = cmd.symbol ? cmd.symbol->value : "";
    const auto result = cmd.kind == CommandKind::Libs ? library_list(query, error) : library_info(query, error);
    std::cout << color_inspection(error.empty() ? result : "error: " + error + '\n',
        inspection_colors_enabled(), cmd.kind == CommandKind::Libs);
    return true;
  }
  if (cmd.kind == CommandKind::Symbols && cmd.error.empty()) {
    std::string error;
    const auto result = cpp.symbol_list(cmd.library_regex ? cmd.library_regex->value : "",
                                         cmd.name_regex ? cmd.name_regex->value : "", error,
                                         cmd.all_symbols.has_value());
    if (result) std::cout << color_inspection(*result, inspection_colors_enabled(), true);
    else std::cout << color_inspection("error: " + error + '\n', inspection_colors_enabled());
    return true;
  }
  if (cmd.kind == CommandKind::Restore && cmd.error.empty()) {
    std::string error;
    const std::string signature = cmd.signature ? cmd.signature->value : "";
    if (cpp.restore(signature, error, cmd.caller ? cmd.caller->value : "")) {
      if (cmd.signature) std::cout << "restored: " << signature
          << (cmd.caller ? " in " + cmd.caller->value : "") << '\n';
      else std::cout << "restored all mocks and hooks\n";
    } else {
      std::cout << "error: " << error << '\n';
    }
    return true;
  }
  if (cmd.kind == CommandKind::Hooks && cmd.error.empty()) {
    const auto hooks = cpp.active_hooks();
    if (hooks.empty()) std::cout << "no active mocks or hooks\n";
    for (const auto& hook : hooks) {
      const char* name = hook.kind == HookKind::Mock ? "%mock" :
                         hook.kind == HookKind::Pre ? "%pre" : "%post";
      std::cout << name << ' ' << hook.signature
                << (hook.caller.empty() ? "" : " in " + hook.caller)
                << " cpp`" << hook.body << "`\n";
    }
    return true;
  }
  if ((cmd.kind == CommandKind::Mock || cmd.kind == CommandKind::Pre ||
       cmd.kind == CommandKind::Post) && cmd.error.empty()) {
    const HookKind kind = cmd.kind == CommandKind::Mock ? HookKind::Mock :
                          cmd.kind == CommandKind::Pre ? HookKind::Pre : HookKind::Post;
    std::string error;
    if (!cpp.set_hook(kind, cmd.signature->value, cmd.code->value, error,
                      cmd.caller ? cmd.caller->value : "")) {
      std::cout << "error: " << error << '\n';
    } else {
      std::cout << cmd.name << (cmd.code->value.empty() ? " cleared: " : " set: ")
                << cmd.signature->value << (cmd.caller ? " in " + cmd.caller->value : "") << '\n';
    }
    return true;
  }
  return dispatch(cmd, libs, std::cout);
}

}  // namespace example
