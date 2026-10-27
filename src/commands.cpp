#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <system_error>
#include <utility>

#include "cpp_lexer.hpp"
#include "inspection_output.hpp"
#include "llvm/Demangle/Demangle.h"

namespace example {

namespace fs = std::filesystem;

namespace {

constexpr const char* kBlank = " \t\r\n";

bool is_blank(char c) {
  return std::string_view(kBlank).find(c) != std::string_view::npos;
}

std::size_t skip_blank(std::string_view text, std::size_t pos) {
  const std::size_t next = text.find_first_not_of(kBlank, pos);
  return next == std::string_view::npos ? text.size() : next;
}

// Keeps only the first error.
void set_error(ParsedCommand& cmd, std::string message, Span span) {
  if (!cmd.error.empty()) return;
  cmd.error = std::move(message);
  cmd.error_span = span;
}

// Scans one path operand starting at `pos` (not a blank): a quoted string, or
// a word ending at a blank or one of `stops`. Returns false on an error.
bool scan_path(std::string_view text, std::size_t& pos, std::string_view stops,
               ParsedCommand& cmd) {
  const std::size_t begin = pos;
  if (text[pos] == '"') {
    const std::size_t close = text.find('"', pos + 1);
    if (close == std::string_view::npos) {
      set_error(cmd, "unterminated quote", {begin, text.size()});
      return false;
    }
    pos = close + 1;
    if (close == begin + 1) {
      set_error(cmd, "empty path", {begin, pos});
      return false;
    }
    cmd.paths.push_back({std::string(text.substr(begin + 1, close - begin - 1)), {begin, pos}});
    return true;
  }
  while (pos < text.size() && !is_blank(text[pos]) &&
         stops.find(text[pos]) == std::string_view::npos) {
    ++pos;
  }
  cmd.paths.push_back({std::string(text.substr(begin, pos - begin)), {begin, pos}});
  return true;
}

// lib := "%lib" { path },  add := "%add" { path }
void parse_paths(std::string_view text, ParsedCommand& cmd) {
  std::size_t pos = cmd.arg_begin;
  while (pos < text.size()) {
    if (!scan_path(text, pos, "", cmd)) return;
    if (pos < text.size() && !is_blank(text[pos])) {  // e.g. "a"b
      set_error(cmd, "expected a blank between paths", {pos, pos + 1});
      return;
    }
    pos = skip_blank(text, pos);
  }
}

// Shell-style grouping and escapes, with no variable expansion or execution.
// The first word is a filename only when it does not start with '-'.
void parse_compile_options(std::string_view text, ParsedCommand& cmd) {
  std::vector<Operand> words;
  auto pos = cmd.arg_begin;
  while (pos < text.size()) {
    const auto begin = pos;
    std::string word;
    char quote = 0;
    while (pos < text.size() && (quote || !is_blank(text[pos]))) {
      const char c = text[pos++];
      if ((c == '\'' || c == '"') && (!quote || quote == c)) {
        quote = quote ? 0 : c;
      } else if (c == '\\' && quote != '\'') {
        if (pos == text.size()) {
          set_error(cmd, "trailing escape", {pos - 1, pos});
          return;
        }
        const char next = text[pos];
        if (!quote || next == '"' || next == '\\' || next == '$' || next == '`')
          word += text[pos++];
        else
          word += c;
      } else
        word += c;
    }
    if (quote) {
      set_error(cmd, "unterminated quote", {begin, text.size()});
      return;
    }
    words.push_back({std::move(word), {begin, pos}});
    pos = skip_blank(text, pos);
  }
  if (cmd.kind == CommandKind::CompileOpts ||
      cmd.kind == CommandKind::UseCompileOpts) {
    if (words.size() > 1) {
      set_error(cmd, cmd.name + " takes one filename", words[1].span);
      return;
    }
    if (words.empty() && cmd.kind == CommandKind::UseCompileOpts) {
      set_error(cmd, "missing reference filename", {text.size(), text.size()});
      return;
    }
    if (!words.empty()) cmd.paths.push_back(std::move(words.front()));
  } else {
    auto first = words.begin();
    if (first != words.end() &&
        (first->value.empty() || first->value.front() != '-')) {
      cmd.paths.push_back(*first++);
    }
    if (first != words.end() &&
        (first->value.empty() || first->value.front() != '-')) {
      set_error(cmd, "expected a compiler flag after the filename",
                first->span);
      return;
    }
    cmd.compile_flags.assign(first, words.end());
    if (cmd.kind == CommandKind::AddCompileOpt && cmd.compile_flags.empty()) {
      set_error(cmd, "missing compiler flags", {text.size(), text.size()});
    }
  }
  if (!cmd.paths.empty() && cmd.paths.front().value.empty())
    set_error(cmd, "empty filename", cmd.paths.front().span);
}

// add-path := "%add-path" path { ":" path }
void parse_add_path(std::string_view text, ParsedCommand& cmd) {
  std::size_t pos = cmd.arg_begin;
  if (pos == text.size()) {
    set_error(cmd, "missing path", {pos, pos});
    return;
  }
  for (;;) {
    if (pos == text.size() || text[pos] == ':') {  // nothing before this ':'
      const std::size_t colon = pos == text.size() ? cmd.separators.back().begin : pos;
      set_error(cmd, "empty path in list", {colon, colon + 1});
      return;
    }
    if (!scan_path(text, pos, ":", cmd)) return;
    pos = skip_blank(text, pos);
    if (pos == text.size()) return;
    if (text[pos] != ':') {
      set_error(cmd, "expected ':' between paths",
                {pos, text.find_last_not_of(kBlank) + 1});
      return;
    }
    cmd.separators.push_back({pos, pos + 1});
    pos = skip_blank(text, pos + 1);
  }
}

bool is_qualified_name(std::string_view word) {
  if (word.substr(0, 2) == "::") word.remove_prefix(2);
  for (;;) {
    const std::size_t sep = word.find("::");
    const std::string_view part = word.substr(0, sep);
    if (part.empty() || std::isdigit(static_cast<unsigned char>(part[0])) != 0) return false;
    for (const char c : part) {
      if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') return false;
    }
    if (sep == std::string_view::npos) return true;
    word.remove_prefix(sep + 2);
  }
}

// class := "%class" [ name ]
void parse_class(std::string_view text, ParsedCommand& cmd) {
  const std::size_t pos = cmd.arg_begin;
  if (pos == text.size()) return;
  std::size_t end = pos;
  while (end < text.size() && !is_blank(text[end])) ++end;
  const std::string_view word = text.substr(pos, end - pos);
  if (!is_qualified_name(word)) {
    set_error(cmd, "expected a class name", {pos, end});
    return;
  }
  cmd.class_name = Operand{std::string(word), {pos, end}};
  const std::size_t next = skip_blank(text, end);
  if (next < text.size()) {
    set_error(cmd, "%class takes one class name", {next, text.find_last_not_of(kBlank) + 1});
  }
}

void parse_members(std::string_view text, ParsedCommand& cmd) {
  auto pos = cmd.arg_begin;
  auto word = [&]() {
    const auto begin = pos;
    while (pos < text.size() && !is_blank(text[pos])) {
      ++pos;
    }
    Operand result{std::string(text.substr(begin, pos - begin)), {begin, pos}};
    pos = skip_blank(text, pos);
    return result;
  };
  if (pos == text.size()) {
    set_error(cmd, "missing record name", {pos, pos});
    return;
  }
  cmd.class_name = word();
  if (!is_qualified_name(cmd.class_name->value)) {
    set_error(cmd, "expected a qualified record name", cmd.class_name->span);
    return;
  }
  while (pos < text.size()) {
    const auto option = word();
    if (option.value != "access" && option.value != "include-parents") {
      set_error(cmd, "expected access or include-parents", option.span);
      return;
    }
    auto& filter = option.value == "access" ? cmd.access_filter : cmd.parents_filter;
    if (filter) {
      set_error(cmd, "duplicate " + option.value + " option", option.span);
      return;
    }
    if (pos == text.size()) {
      set_error(cmd, "missing value after " + option.value, option.span);
      return;
    }
    filter = word();
    if (option.value == "access") {
      if (filter->value == "all") {
        cmd.member_options.access = AccessFilter::All;
      } else if (filter->value == "public") {
        cmd.member_options.access = AccessFilter::Public;
      } else if (filter->value == "private") {
        cmd.member_options.access = AccessFilter::Private;
      } else if (filter->value == "protected") {
        cmd.member_options.access = AccessFilter::Protected;
      } else {
        set_error(cmd, "expected public, private, protected or all", filter->span);
        return;
      }
    } else if (filter->value == "true" || filter->value == "false") {
      cmd.member_options.include_parents = filter->value == "true";
    } else {
      set_error(cmd, "expected true or false", filter->span);
      return;
    }
  }
}

// Quotes group regexes containing spaces; preserve backslashes for std::regex.
void parse_symbol_filters(std::string_view text, ParsedCommand& cmd) {
  std::size_t pos = cmd.arg_begin;
  while (pos < text.size()) {
    const auto begin = pos;
    auto end = text.find_first_of(kBlank, pos);
    if (end == std::string_view::npos) end = text.size();
    const auto flag = text.substr(pos, end - pos);
    if (flag == "all") {
      if (cmd.all_symbols) {
        set_error(cmd, "duplicate all option", {begin, end});
        return;
      }
      cmd.all_symbols = Span{begin, end};
      pos = skip_blank(text, end);
      continue;
    }
    if (flag == "undefined") {
      if (cmd.undefined_symbols) {
        set_error(cmd, "duplicate undefined option", {begin, end});
        return;
      }
      cmd.undefined_symbols = Span{begin, end};
      pos = skip_blank(text, end);
      continue;
    }
    if (flag != "lib" && flag != "name") {
      set_error(cmd, "expected all, undefined, lib <regex> or name <regex>", {begin, end});
      return;
    }
    auto& filter = flag == "lib" ? cmd.library_regex : cmd.name_regex;
    if (filter) {
      set_error(cmd, "duplicate " + std::string(flag) + " filter", {begin, end});
      return;
    }
    pos = skip_blank(text, end);
    if (pos == text.size()) {
      set_error(cmd, "missing regex after " + std::string(flag), {begin, end});
      return;
    }
    const auto value_begin = pos;
    if (text[pos] == '\'' || text[pos] == '"') {
      const auto close = text.find(text[pos], pos + 1);
      if (close == std::string_view::npos) {
        set_error(cmd, "unterminated regex quote", {pos, text.size()});
        return;
      }
      filter = Operand{std::string(text.substr(pos + 1, close - pos - 1)), {pos, close + 1}};
      pos = close + 1;
      if (pos < text.size() && !is_blank(text[pos])) {
        set_error(cmd, "expected a blank after regex", {pos, pos + 1});
        return;
      }
    } else {
      end = text.find_first_of(kBlank, pos);
      if (end == std::string_view::npos) end = text.size();
      filter = Operand{std::string(text.substr(pos, end - pos)), {value_begin, end}};
      pos = end;
    }
    pos = skip_blank(text, pos);
  }
}

void parse_session_option(std::string_view text, ParsedCommand& cmd) {
  std::size_t pos = cmd.arg_begin;
  if (pos == text.size()) {
    set_error(cmd, "missing option assignment", {pos, pos});
    return;
  }
  const auto name_begin = pos;
  while (pos < text.size() && !is_blank(text[pos]) && text[pos] != '=') {
    ++pos;
  }
  cmd.option_name = std::string(text.substr(name_begin, pos - name_begin));
  if (std::find(repl_option_names.begin(), repl_option_names.end(), cmd.option_name) ==
      repl_option_names.end()) {
    set_error(cmd, "unknown option '" + cmd.option_name + "'", {name_begin, pos});
    return;
  }
  pos = skip_blank(text, pos);
  if (pos == text.size() || text[pos] != '=') {
    set_error(cmd, "expected '=' after option name", {pos, pos});
    return;
  }
  cmd.option_equals = Span{pos, pos + 1};
  pos = skip_blank(text, pos + 1);
  if (pos == text.size()) {
    set_error(cmd, "missing boolean value", {pos, pos});
    return;
  }
  const auto value_begin = pos;
  while (pos < text.size() && !is_blank(text[pos])) {
    ++pos;
  }
  cmd.option_value =
      Operand{std::string(text.substr(value_begin, pos - value_begin)), {value_begin, pos}};
  if (cmd.option_value->value != "true" && cmd.option_value->value != "false") {
    set_error(cmd, "expected true or false", cmd.option_value->span);
    return;
  }
  pos = skip_blank(text, pos);
  if (pos < text.size()) {
    set_error(cmd, "%set takes one option assignment", {pos, text.size()});
  }
}

void parse_inspection_operand(std::string_view text, ParsedCommand& cmd, bool required,
                              std::size_t begin) {
  if (begin == text.size()) {
    if (required) set_error(cmd, cmd.kind == CommandKind::LibInfo ? "missing library name" : "missing symbol name", {begin, begin});
    return;
  }
  auto end = text.find_first_of(kBlank, begin);
  if (end == std::string_view::npos) end = text.size();
  std::string value;
  if (text[begin] == '\'' || text[begin] == '"') {
    const auto close = text.find(text[begin], begin + 1);
    if (close == std::string_view::npos) { set_error(cmd, "unterminated quote", {begin, text.size()}); return; }
    end = close + 1;
    value = std::string(text.substr(begin + 1, close - begin - 1));
  } else value = std::string(text.substr(begin, end - begin));
  cmd.symbol = Operand{std::move(value), {begin, end}};
  if ((cmd.kind == CommandKind::Demangle || cmd.kind == CommandKind::DefineVariable) &&
      cmd.symbol->value.empty())
    set_error(cmd, "missing symbol name", {begin, end});
  const auto extra = skip_blank(text, end);
  if (extra < text.size()) set_error(cmd, cmd.name + " takes one " +
      ((cmd.kind == CommandKind::Info || cmd.kind == CommandKind::Demangle ||
        cmd.kind == CommandKind::DefineVariable) ?
          "symbol name" : "library name or regex"), {extra, text.size()});
}

void parse_signature(std::string_view text, std::size_t begin, std::size_t end,
                     ParsedCommand& cmd) {
  const auto value = text.substr(begin, end - begin);
  int depth = 0;
  for (const auto& token : lex_cpp(value).tokens) {
    if (token.kind == TokenKind::Bracket) {
      if (value[token.begin] == '(' || value[token.begin] == '[' || value[token.begin] == '{') ++depth;
      else --depth;
      continue;
    }
    if (depth != 0 || token.kind != TokenKind::Identifier ||
        value.substr(token.begin, token.end - token.begin) != "in" ||
        (token.begin > 0 && !is_blank(value[token.begin - 1])) ||
        (token.end < value.size() && !is_blank(value[token.end]))) continue;
    const auto target_end = value.find_last_not_of(kBlank, token.begin == 0 ? 0 : token.begin - 1);
    const auto caller_begin = skip_blank(value, token.end);
    // An ordinary function or namespace may itself be named "in".
    if (caller_begin < value.size() && (value[caller_begin] == '(' || value[caller_begin] == ':')) continue;
    if (token.begin == 0 || caller_begin == value.size()) {
      set_error(cmd, "expected <target signature> in <caller signature>", {begin, end});
      return;
    }
    cmd.signature = Operand{std::string(value.substr(0, target_end + 1)),
                            {begin, begin + target_end + 1}};
    cmd.caller = Operand{std::string(value.substr(caller_begin)), {begin + caller_begin, end}};
    return;
  }
  cmd.signature = Operand{std::string(value), {begin, end}};
}

// Backticks inside C++ strings, raw strings, chars and comments are data.
std::size_t cpp_block_close(std::string_view text, std::size_t begin) {
  for (const Token& token : lex_cpp(text.substr(begin)).tokens) {
    if (token.kind == TokenKind::String || token.kind == TokenKind::Char ||
        token.kind == TokenKind::Escape || token.kind == TokenKind::Comment ||
        token.kind == TokenKind::Preprocessor) continue;
    for (std::size_t i = token.begin; i < token.end; ++i)
      if (text[begin + i] == '`') return begin + i;
  }
  return std::string_view::npos;
}

void parse_hook(std::string_view text, ParsedCommand& cmd) {
  std::size_t marker = text.find("cpp`", cmd.arg_begin);
  while (marker != std::string_view::npos && marker > cmd.arg_begin &&
         !is_blank(text[marker - 1])) marker = text.find("cpp`", marker + 4);
  if (marker == std::string_view::npos) {
    set_error(cmd, cmd.kind == CommandKind::DefineVariable ?
        "expected <symbol> cpp`<C++ initializer>`" :
        "expected <signature> cpp`<C++ body>`", {cmd.arg_begin, text.size()});
    return;
  }
  const std::size_t end = text.find_last_not_of(kBlank, marker - 1);
  if (marker == cmd.arg_begin || end == std::string_view::npos || end < cmd.arg_begin) {
    set_error(cmd, cmd.kind == CommandKind::DefineVariable ?
        "missing variable name" : "missing function signature", {cmd.arg_begin, marker});
    return;
  }
  if (cmd.kind == CommandKind::DefineVariable) {
    const auto operand = text.substr(cmd.arg_begin, end + 1 - cmd.arg_begin);
    if (operand.find('(') != std::string_view::npos) {
      cmd.signature = Operand{std::string(operand), {cmd.arg_begin, end + 1}};
    } else {
      parse_inspection_operand(text.substr(0, end + 1), cmd, true, cmd.arg_begin);
    }
  } else {
    parse_signature(text, cmd.arg_begin, end + 1, cmd);
  }
  if (!cmd.error.empty()) return;
  const std::size_t begin = marker + 4;
  const auto close = cpp_block_close(text, begin);
  if (close == std::string_view::npos) {
    cmd.incomplete = true;
    cmd.code = Operand{std::string(text.substr(begin)), {begin, text.size()}};
    set_error(cmd, "unterminated cpp` block", {marker, text.size()});
    return;
  }
  cmd.code = Operand{std::string(text.substr(begin, close - begin)), {begin, close}};
  if (cmd.kind == CommandKind::DefineVariable && !cmd.signature &&
      cmd.code->value.find_first_not_of(kBlank) == std::string::npos) {
    set_error(cmd, "missing C++ initializer", {begin, close});
  }
  const std::size_t next = skip_blank(text, close + 1);
  if (next < text.size()) set_error(cmd, "unexpected text after cpp` block", {next, text.size()});
}

}  // namespace

const std::vector<CommandInfo>& command_table() {
  static const std::vector<CommandInfo> kTable = {
      {"%lib", CommandKind::Lib, " [path ...]",
       "load shared libraries (no argument: list loaded libraries)"},
      {"%libs", CommandKind::Libs, " [all] [regex]",
       "list loaded library names (all includes system libraries)"},
      {"%libinfo", CommandKind::LibInfo, " <library>",
       "show library path, versions, ABI requirements and dependencies"},
      {"%paths", CommandKind::Paths, "", "show library search paths and loader settings"},
      {"%add-path", CommandKind::AddPath, " <path[:path...]>",
       "add library search directories, separated by ':'"},
      {"%add", CommandKind::Add, " [path ...]",
       "parse and run C++ files using compile_commands.json (no argument: list "
       "added files)"},
      {"%reload", CommandKind::Reload, " [path ...]",
       "reload added C++ files and user headers (no argument: reload all)"},
      {"%compile-opts", CommandKind::CompileOpts, " [file]",
       "show global or effective file compile options"},
      {"%add-compile-opt", CommandKind::AddCompileOpt, " [file] <flags...>",
       "append global or file compile flags"},
      {"%set-compile-opts", CommandKind::SetCompileOpts, " [file] [flags...]",
       "replace global or file compile flags (no flags: clear)"},
      {"%use-compile-opts", CommandKind::UseCompileOpts, " <file>",
       "use a file's effective compile options as the session defaults"},
      {"%set", CommandKind::Set, " <option> = <true|false>",
       "set a session option (print_values or keep_hints)"},
      {"%class", CommandKind::Class, " [name]",
       "show a class's members and layout (no argument: list classes)"},
      {"%methods", CommandKind::Methods,
       " <name> [access public|private|protected|all] [include-parents true|false]",
       "list record methods by declaring record (defaults: all access, no parents)"},
      {"%fields", CommandKind::Fields,
       " <name> [access public|private|protected|all] [include-parents true|false]",
       "list record data members by declaring record (defaults: all access, no parents)"},
      {"%info", CommandKind::Info, " <symbol>",
       "show symbol declarations, types, locations and JIT addresses"},
      {"%syminfo", CommandKind::Info, " <symbol>",
       "show symbol definitions, imports and reference sites"},
      {"%type", CommandKind::Type, " <symbol>",
       "print a symbol's declared type (functions: (parameters) -> result)"},
      {"!", CommandKind::Shell, " <shell-command>",
       "run one shell line and print stdout and stderr"},
      {"%demangle", CommandKind::Demangle, " <symbol>",
       "print a readable C++ symbol name without loading its library"},
      {"%define", CommandKind::DefineVariable, " <symbol or signature> cpp`<initializer or body>`",
       "define missing variable storage or a callable before loading a library"},
      {"%symbols", CommandKind::Symbols, " [all] [undefined] [lib <regex>] [name <regex>]",
       "list loaded symbol names (all includes system libraries)"},
      {"%mock", CommandKind::Mock, " <signature> [in <caller>] cpp`<body>`",
       "replace a function or method body"},
      {"%pre", CommandKind::Pre, " <signature> [in <caller>] cpp`<body>`",
       "run before a function or method call"},
      {"%post", CommandKind::Post, " <signature> [in <caller>] cpp`<body>`",
       "run after a call (result is available)"},
      {"%restore", CommandKind::Restore, " [signature [in <caller>]]",
       "remove mocks and hooks (no argument: restore the whole session)"},
      {"%hooks", CommandKind::Hooks, "",
       "list active mocks and hooks with signatures and C++ bodies"},
      {"%clear", CommandKind::Clear, "", "clear the screen"},
      {"%help", CommandKind::Help, "", "show this help"},
      {"%quit", CommandKind::Quit, "", "exit the REPL"},
  };
  return kTable;
}

const CommandInfo* find_command(std::string_view name) {
  for (const CommandInfo& c : command_table()) {
    if (name == c.name) return &c;
  }
  return nullptr;
}

std::vector<std::string> command_names() {
  std::vector<std::string> names;
  for (const CommandInfo& c : command_table()) names.emplace_back(c.name);
  return names;
}

ParsedCommand parse_command(std::string_view text, bool allow_shell) {
  ParsedCommand cmd;
  const std::size_t start = text.find_first_not_of(kBlank);
  if (start == std::string_view::npos) {
    return cmd;
  }
  if (text[start] == '!') {
    cmd.kind = CommandKind::Shell;
    cmd.name = "!";
    cmd.name_begin = start;
    cmd.name_end = start + 1;
    const auto argument = text.find_first_not_of(" \t", start + 1);
    cmd.arg_begin = argument == std::string_view::npos ? text.size() : argument;
    cmd.code = Operand{std::string(text.substr(cmd.arg_begin)), {cmd.arg_begin, text.size()}};
    if (cmd.code->value.empty())
      set_error(cmd, "missing shell command", {text.size(), text.size()});
    else if (cmd.code->value.find_first_of("\r\n") != std::string::npos)
      set_error(cmd, "shell commands must fit on one line", cmd.code->span);
    else if (cmd.code->value.find('\0') != std::string::npos)
      set_error(cmd, "shell command contains a NUL byte", cmd.code->span);
    return cmd;
  }
  if (text[start] == '$') {
    cmd.kind = CommandKind::OptionValue;
    cmd.name_begin = start;
    cmd.arg_begin = start + 1;
    std::size_t end = text.find_first_of(kBlank, cmd.arg_begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    cmd.option_name = std::string(text.substr(cmd.arg_begin, end - cmd.arg_begin));
    cmd.name = "$" + cmd.option_name;
    cmd.name_end = end;
    if (skip_blank(text, end) < text.size()) {
      set_error(cmd, "option query takes one name", {skip_blank(text, end), text.size()});
    }
    return cmd;
  }
  if (text[start] != '%') {
    return cmd;
  }

  std::size_t end = text.find_first_of(kBlank, start);
  if (end == std::string_view::npos) end = text.size();
  cmd.name = std::string(text.substr(start, end - start));
  cmd.name_begin = start;
  cmd.name_end = end;

  cmd.arg_begin = skip_blank(text, end);

  const CommandInfo* info = find_command(cmd.name);
  cmd.kind = info != nullptr ? info->kind : CommandKind::Unknown;
  if (info && allow_shell && cmd.kind != CommandKind::Set) {
    auto pos = cmd.arg_begin;
    while (true) {
      const auto begin = text.find("``", pos);
      // A cpp` body belongs to the C++ lexer, including empty cpp`` bodies
      // and strings/comments containing pairs of backticks.
      if (cmd.kind == CommandKind::Mock || cmd.kind == CommandKind::Pre ||
          cmd.kind == CommandKind::Post || cmd.kind == CommandKind::DefineVariable) {
        auto marker = text.find("cpp`", pos);
        while (marker != std::string_view::npos && marker > cmd.arg_begin &&
               !is_blank(text[marker - 1]))
          marker = text.find("cpp`", marker + 4);
        if (marker != std::string_view::npos &&
            (begin == std::string_view::npos || marker < begin)) {
          if (!cmd.shell_commands.empty() &&
              cpp_block_close(text, marker + 4) == std::string_view::npos) {
            cmd.incomplete = true;
            set_error(cmd, "unterminated cpp` block", {marker, text.size()});
          }
          break;
        }
      }
      if (begin == std::string_view::npos) break;
      cmd.original_text = std::string(text);
      const auto close = text.find("``", begin + 2);
      if (close == std::string_view::npos) {
        cmd.incomplete = true;
        set_error(cmd, "unterminated shell `` block", {begin, text.size()});
        return cmd;
      }
      cmd.shell_commands.push_back(
          {std::string(text.substr(begin + 2, close - begin - 2)),
           {begin, close + 2}});
      pos = close + 2;
    }
    if (!cmd.shell_commands.empty()) return cmd;
  }
  switch (cmd.kind) {
    case CommandKind::Cpp:
    case CommandKind::OptionValue:
    case CommandKind::Shell:
      break;
    case CommandKind::Unknown:
      set_error(cmd, "unknown command '" + cmd.name + "' (try %help)", {start, end});
      break;
    case CommandKind::Lib:
      parse_paths(text, cmd);
      break;
    case CommandKind::AddPath:
      parse_add_path(text, cmd);
      break;
    case CommandKind::Add:
    case CommandKind::Reload:
      parse_paths(text, cmd);
      break;
    case CommandKind::CompileOpts:
    case CommandKind::AddCompileOpt:
    case CommandKind::SetCompileOpts:
    case CommandKind::UseCompileOpts:
      parse_compile_options(text, cmd);
      break;
    case CommandKind::Set:
      parse_session_option(text, cmd);
      break;
    case CommandKind::Class:
      parse_class(text, cmd);
      break;
    case CommandKind::Methods:
    case CommandKind::Fields:
      parse_members(text, cmd);
      break;
    case CommandKind::Info:
    case CommandKind::Type:
    case CommandKind::Demangle:
    case CommandKind::LibInfo:
      parse_inspection_operand(text, cmd, true, cmd.arg_begin);
      break;
    case CommandKind::Libs: {
      auto begin = cmd.arg_begin;
      auto end = text.find_first_of(kBlank, begin);
      if (end == std::string_view::npos) end = text.size();
      if (text.substr(begin, end - begin) == "all") {
        cmd.all_libraries = Span{begin, end};
        begin = skip_blank(text, end);
      }
      parse_inspection_operand(text, cmd, false, begin);
      break;
    }
    case CommandKind::Symbols:
      parse_symbol_filters(text, cmd);
      break;
    case CommandKind::Mock:
    case CommandKind::Pre:
    case CommandKind::Post:
    case CommandKind::DefineVariable:
      parse_hook(text, cmd);
      break;
    case CommandKind::Restore: {
      const auto begin = cmd.arg_begin;
      if (begin != text.size()) {
        const auto end = text.find_last_not_of(kBlank) + 1;
        parse_signature(text, begin, end, cmd);
      }
      break;
    }
    case CommandKind::Hooks:
    case CommandKind::Paths:
    case CommandKind::Clear:
    case CommandKind::Help:
    case CommandKind::Quit:
      if (cmd.arg_begin < text.size()) {
        set_error(cmd, cmd.name + " takes no arguments",
                  {cmd.arg_begin, text.find_last_not_of(kBlank) + 1});
      }
      break;
  }
  return cmd;
}

bool is_command_line(std::string_view text) {
  const std::size_t start = text.find_first_not_of(kBlank);
  return start != std::string_view::npos &&
         (text[start] == '%' || text[start] == '$' || text[start] == '!');
}

bool wants_more_input(std::string_view text) {
  return is_command_line(text) ? parse_command(text).incomplete : is_incomplete(text);
}

bool dispatch(const ParsedCommand& cmd, LibraryManager& libs, std::ostream& out,
              bool* ok) {
  if (ok) *ok = true;
  if (!cmd.error.empty()) {
    if (ok) *ok = false;
    out << "error: " << cmd.error << '\n';
    if (const CommandInfo* info = find_command(cmd.name)) {
      out << "usage: " << info->name << info->usage << '\n';
    }
    return true;
  }
  if (!cmd.shell_commands.empty())
    return dispatch(expand_shell_substitutions(cmd), libs, out, ok);
  switch (cmd.kind) {
    case CommandKind::Cpp:
    case CommandKind::Set:
    case CommandKind::OptionValue:
      break;
    case CommandKind::Quit:
      return false;
    case CommandKind::Shell: {
      std::string error;
      const bool success = execute_shell(cmd.code->value, out, error);
      if (ok) *ok = success;
      if (!success) out << "error: " << error << '\n';
      break;
    }
    case CommandKind::Clear:
      out << "\x1b[2J\x1b[H" << std::flush;
      break;
    case CommandKind::Demangle: {
      const auto& original = cmd.symbol->value;
      // Mach-O symbol tables can retain the extra platform underscore.
      const auto mangled = original.rfind("__Z", 0) == 0
                               ? original.substr(1) : original;
      const auto readable = llvm::demangle(mangled);
      out << color_inspection((readable == mangled ? original : readable) + '\n',
                              inspection_colors_enabled(), true);
      break;
    }
    case CommandKind::Help: {
      std::size_t width = 0;
      for (const CommandInfo& c : command_table()) {
        width = std::max(width, std::string(c.name).size() + std::string(c.usage).size());
      }
      out << "Commands:\n";
      for (const CommandInfo& c : command_table()) {
        out << "  " << std::left << std::setw(static_cast<int>(width + 2))
            << (std::string(c.name) + c.usage) << c.description << '\n';
      }
      out << "Other input is C++; expression values print by default. "
             "%set print_values = false disables automatic output.\n"
             "Incomplete C++ continues on the next line, including blank lines; "
             "Ctrl+C cancels it.\n"
             "Hooks accept multiline cpp`...` blocks; an empty body clears a hook.\n"
             "Most command arguments accept multiline ``shell script`` blocks; "
             "stdout replaces each block before the command runs. %set and "
             "$option never run shell substitutions.\n"
             "Order: pre, mock/original, post; arguments are named in the signature, "
             "methods use self/this, post uses result.\n"
             "Keys: Tab complete, Ctrl+J newline, Ctrl+L clear screen, "
             "Ctrl+C cancel, Ctrl+D exit\n"
             "Overload menu: Up/Down select, Tab accept, Escape close.\n";
      break;
    }
    case CommandKind::AddPath:
      for (const Operand& p : cmd.paths) {
        const auto r = libs.add_path(p.value);
        if (ok && !r.ok) *ok = false;
        out << (r.ok ? "" : "error: ") << r.message << '\n';
      }
      break;
    case CommandKind::Paths:
      out << color_inspection(libs.describe_search_paths(), inspection_colors_enabled());
      break;
    case CommandKind::Lib:
      if (cmd.paths.empty()) {
        for (const auto& lib : libs.loaded()) out << color_inspection(fs::path(lib.path).filename().string() + '\n', inspection_colors_enabled(), true);
      }
      for (const Operand& p : cmd.paths) {
        const auto r = libs.load(p.value);
        if (ok && !r.ok) *ok = false;
        out << (r.ok ? "" : "error: ") << r.message << '\n';
      }
      break;
    case CommandKind::Add:      // %add and %class are run by the REPL, which
    case CommandKind::Reload:
    case CommandKind::CompileOpts:
    case CommandKind::AddCompileOpt:
    case CommandKind::SetCompileOpts:
    case CommandKind::UseCompileOpts:
    case CommandKind::DefineVariable:
    case CommandKind::Class:    // owns the interpreter
    case CommandKind::Methods:
    case CommandKind::Fields:
    case CommandKind::Info:
    case CommandKind::Type:
    case CommandKind::Libs:
    case CommandKind::LibInfo:
    case CommandKind::Symbols:
    case CommandKind::Mock:
    case CommandKind::Pre:
    case CommandKind::Post:
    case CommandKind::Restore:
    case CommandKind::Hooks:
    case CommandKind::Unknown:  // always carries an error, handled above
      break;
  }
  return true;
}

std::vector<std::string> complete_path(std::string_view typed, bool dirs_only) {
  if (typed == "~") return {"~/"};

  const std::size_t slash = typed.rfind('/');
  const std::string dir_typed =
      slash == std::string_view::npos ? "" : std::string(typed.substr(0, slash + 1));
  const std::string prefix(typed.substr(slash == std::string_view::npos ? 0 : slash + 1));

  std::error_code ec;
  fs::directory_iterator it(dir_typed.empty() ? fs::path(".") : fs::path(expand_home(dir_typed)), ec);
  std::vector<std::string> out;
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    if (name[0] == '.' && prefix.empty()) continue;  // hide dotfiles by default
    std::error_code type_ec;
    const bool is_dir = it->is_directory(type_ec);
    if (dirs_only && !is_dir) continue;
    out.push_back(dir_typed + name + (is_dir ? "/" : ""));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t utf8_length(std::string_view s) {
  return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}

}  // namespace example
