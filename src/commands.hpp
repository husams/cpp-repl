#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "library_manager.hpp"

namespace example {

// A line whose first non-blank character is '%' is a REPL command; everything
// else is C++. Command grammar (blanks separate words):
//
//   command   := lib | libs | libinfo | add-path | add | class | info | symbols | hook | hooks | restore | help | quit
//   lib       := "%lib" { path }                 zero or more paths
//   libs      := "%libs" [ "all" ] [ regex ]     loaded library names only
//   paths     := "%paths"                       current library search paths
//   libinfo   := "%libinfo" library               name, path or regex
//   add-path  := "%add-path" path { ":" path }   blanks around ':' allowed
//   add       := "%add" { path }                 zero or more source files
//   class     := "%class" [ name ]               name := ident { "::" ident }
//   info      := "%info" symbol                  qualified or mangled name
//   symbols   := "%symbols" [ "all" ] [ "lib" regex ] [ "name" regex ]
//   hook      := ("%mock" | "%pre" | "%post") signature [ "in" signature ] "cpp`" body "`"
//   restore   := "%restore" [ signature [ "in" signature ] ]
//   hooks     := "%hooks"
//   help      := "%help"
//   quit      := "%quit"
//   path      := '"' { any char but '"' } '"'    quoted, may contain blanks
//              | word                            no blanks (and no ':' in
//                                                %add-path)
enum class CommandKind { Cpp, Lib, Libs, LibInfo, Paths, AddPath, Add, Class, Info, Symbols, Mock, Pre, Post, Restore, Hooks, Help, Quit, Unknown };

struct CommandInfo {
  const char* name;  // including the leading '%'
  CommandKind kind;
  const char* usage;  // argument synopsis with a leading space, or ""
  const char* description;
};

const std::vector<CommandInfo>& command_table();
const CommandInfo* find_command(std::string_view name);
std::vector<std::string> command_names();

// Byte range [begin, end) in the parsed text.
struct Span {
  std::size_t begin = 0;
  std::size_t end = 0;
};

// A path or class name operand.
struct Operand {
  std::string value;  // without the surrounding quotes
  Span span;          // including the quotes
};

struct ParsedCommand {
  CommandKind kind = CommandKind::Cpp;
  std::string name;  // "%lib", "%foo", ...; empty for Cpp
  // Byte offsets into the parsed text (valid for non-Cpp kinds).
  std::size_t name_begin = 0;
  std::size_t name_end = 0;
  std::size_t arg_begin = 0;  // text.size() when there is no argument
  std::vector<Operand> paths;         // %lib / %add-path / %add operands
  std::vector<Span> separators;       // the ':' between %add-path operands
  std::optional<Operand> class_name;  // %class operand
  std::optional<Operand> symbol;      // %info operand
  std::optional<Operand> library_regex;  // %symbols lib
  std::optional<Operand> name_regex;     // %symbols name
  std::optional<Span> all_symbols;       // %symbols all (include system libraries)
  std::optional<Span> all_libraries;     // %libs all (include system libraries)
  std::optional<Operand> signature;   // %mock / %pre / %post / %restore target
  std::optional<Operand> caller;      // optional direct caller after "in"
  std::optional<Operand> code;        // contents of cpp`...`
  bool incomplete = false;            // an open cpp` block
  // First grammar violation; empty when the command is well formed.
  std::string error;
  Span error_span;  // the offending text (may be empty, e.g. a missing path)
};

ParsedCommand parse_command(std::string_view text);

// True if the first non-blank character is '%'.
bool is_command_line(std::string_view text);

// True if Enter should continue the input instead of submitting it. Commands
// with an open cpp` block continue; C++ continues while is_incomplete().
bool wants_more_input(std::string_view text);

// Runs a parsed command, writing all output (errors prefixed "error: ") to
// `out`. A command with a grammar error only prints the error and its usage.
// Returns false when the REPL should exit (%quit). Cpp kind is a no-op, and so
// are interpreter commands (%add, %class and hooks): the REPL runs them.
bool dispatch(const ParsedCommand& cmd, LibraryManager& libs, std::ostream& out);

// Filesystem completions for a partially typed path (keeps the typed
// directory prefix, appends '/' to directories).
std::vector<std::string> complete_path(std::string_view typed, bool dirs_only);

// Number of Unicode code points in a UTF-8 string.
std::size_t utf8_length(std::string_view s);

}  // namespace example
