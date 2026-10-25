#include "session_commands.hpp"

#include <ostream>
#include <string>
#include <vector>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "inspection_output.hpp"
#include "runtime_inspection.hpp"

namespace example {
namespace {

// %add [path ...]: adds each source file, or lists the added ones.
bool add_sources(CppInterpreter& cpp, const ParsedCommand& cmd,
                 std::ostream& out) {
  if (cmd.paths.empty()) {
    if (cpp.sources().empty()) out << "no source files added\n";
    for (const std::string& file : cpp.sources()) out << file << '\n';
    return true;
  }
  bool ok = true;
  for (const Operand& p : cmd.paths) {
    const CppInterpreter::AddResult r = cpp.add_source(p.value);
    ok = r.ok && ok;
    out << (r.ok ? "" : "error: ") << r.message << '\n';
  }
  return ok;
}

// %class [name]: lists the session's classes, or describes one.
bool show_class(CppInterpreter& cpp, const ParsedCommand& cmd,
                std::ostream& out) {
  if (!cmd.class_name) {
    const std::vector<std::string> names = cpp.class_names();
    if (names.empty()) out << "no classes defined\n";
    for (const std::string& name : names) out << name << '\n';
    return true;
  }
  std::string error;
  if (const auto synopsis = cpp.describe_class(cmd.class_name->value, error)) {
    out << *synopsis;
  } else {
    out << "error: " << error << '\n';
    return false;
  }
  return true;
}

}  // namespace

CommandResult execute_command(LibraryManager& libs, CppInterpreter& cpp,
                              const ParsedCommand& cmd, std::ostream& out) {
  if (cmd.kind == CommandKind::Cpp) {
    return {true, false};  // C++ input is handled by the interactive REPL.
  }
  if (cmd.kind == CommandKind::DefineVariable && cmd.error.empty()) {
    std::string error;
    const bool ok =
        cpp.define_variable(cmd.symbol->value, cmd.code->value, error);
    out << color_inspection(
        ok ? "defined: " + cmd.symbol->value + '\n' : "error: " + error + '\n',
        inspection_colors_enabled());
    return {true, ok};
  }
  if ((cmd.kind == CommandKind::CompileOpts ||
       cmd.kind == CommandKind::AddCompileOpt ||
       cmd.kind == CommandKind::SetCompileOpts ||
       cmd.kind == CommandKind::UseCompileOpts) &&
      cmd.error.empty()) {
    const auto file = cmd.paths.empty() ? "" : cmd.paths.front().value;
    std::string error;
    if (cmd.kind == CommandKind::CompileOpts) {
      const auto result = cpp.describe_compile_options(error, file);
      out << color_inspection(result ? *result : "error: " + error + '\n',
                              inspection_colors_enabled());
      return {true, result.has_value()};
    } else if (cmd.kind == CommandKind::UseCompileOpts) {
      const bool updated = cpp.use_compile_options(file, error);
      out << color_inspection(
          updated ? "Session compile options updated:\n  Reference file : " +
                        file + "\n  Applies to     : future compilation\n"
                  : "error: " + error + '\n',
          inspection_colors_enabled());
      return {true, updated};
    } else {
      std::vector<std::string> flags;
      for (const auto& flag : cmd.compile_flags) flags.push_back(flag.value);
      const bool updated = cmd.kind == CommandKind::AddCompileOpt
                               ? cpp.add_compile_options(flags, error, file)
                               : cpp.set_compile_options(flags, error, file);
      const auto result =
          updated ? "Compile options updated:\n  Scope      : " +
                        (file.empty() ? std::string("global") : file) +
                        "\n  Action     : " +
                        (cmd.kind == CommandKind::AddCompileOpt ? "appended"
                                                                : "replaced") +
                        "\n  Applies to : future compilation\n"
                  : "error: " + error + '\n';
      out << color_inspection(result, inspection_colors_enabled());
      return {true, updated};
    }
  }
  if (cmd.kind == CommandKind::Add && cmd.error.empty()) {
    return {true, add_sources(cpp, cmd, out)};
  }
  if (cmd.kind == CommandKind::Class && cmd.error.empty()) {
    return {true, show_class(cpp, cmd, out)};
  }
  if (cmd.kind == CommandKind::Info && cmd.error.empty()) {
    std::string error;
    if (const auto info = cpp.symbol_info(cmd.symbol->value, error))
      out << color_inspection(*info, inspection_colors_enabled());
    else
      out << color_inspection("error: " + error + '\n',
                              inspection_colors_enabled());
    return {true, error.empty()};
  }
  if ((cmd.kind == CommandKind::Libs || cmd.kind == CommandKind::LibInfo) &&
      cmd.error.empty()) {
    std::string error;
    const auto query = cmd.symbol ? cmd.symbol->value : "";
    const auto result =
        cmd.kind == CommandKind::Libs
            ? library_list(query, error, cmd.all_libraries.has_value())
            : library_info(query, error);
    out << color_inspection(error.empty() ? result : "error: " + error + '\n',
                            inspection_colors_enabled(),
                            cmd.kind == CommandKind::Libs);
    return {true, error.empty()};
  }
  if (cmd.kind == CommandKind::Symbols && cmd.error.empty()) {
    std::string error;
    const auto result = cpp.symbol_list(
        cmd.library_regex ? cmd.library_regex->value : "",
        cmd.name_regex ? cmd.name_regex->value : "", error,
        cmd.all_symbols.has_value(), cmd.undefined_symbols.has_value());
    if (result)
      out << color_inspection(*result, inspection_colors_enabled(), true);
    else
      out << color_inspection("error: " + error + '\n',
                              inspection_colors_enabled());
    return {true, result.has_value()};
  }
  if (cmd.kind == CommandKind::Restore && cmd.error.empty()) {
    std::string error;
    const std::string signature = cmd.signature ? cmd.signature->value : "";
    if (cpp.restore(signature, error, cmd.caller ? cmd.caller->value : "")) {
      if (cmd.signature)
        out << "restored: " << signature
            << (cmd.caller ? " in " + cmd.caller->value : "") << '\n';
      else
        out << "restored all mocks and hooks\n";
    } else {
      out << "error: " << error << '\n';
    }
    return {true, error.empty()};
  }
  if (cmd.kind == CommandKind::Hooks && cmd.error.empty()) {
    const auto hooks = cpp.active_hooks();
    if (hooks.empty()) out << "no active mocks or hooks\n";
    for (const auto& hook : hooks) {
      const char* name = hook.kind == HookKind::Mock  ? "%mock"
                         : hook.kind == HookKind::Pre ? "%pre"
                                                      : "%post";
      out << name << ' ' << hook.signature
          << (hook.caller.empty() ? "" : " in " + hook.caller) << " cpp`"
          << hook.body << "`\n";
    }
    return {};
  }
  if ((cmd.kind == CommandKind::Mock || cmd.kind == CommandKind::Pre ||
       cmd.kind == CommandKind::Post) &&
      cmd.error.empty()) {
    const HookKind kind = cmd.kind == CommandKind::Mock  ? HookKind::Mock
                          : cmd.kind == CommandKind::Pre ? HookKind::Pre
                                                         : HookKind::Post;
    std::string error;
    if (!cpp.set_hook(kind, cmd.signature->value, cmd.code->value, error,
                      cmd.caller ? cmd.caller->value : "")) {
      out << "error: " << error << '\n';
    } else {
      out << cmd.name << (cmd.code->value.empty() ? " cleared: " : " set: ")
          << cmd.signature->value
          << (cmd.caller ? " in " + cmd.caller->value : "") << '\n';
    }
    return {true, error.empty()};
  }
  bool ok = true;
  const bool keep_running = dispatch(cmd, libs, out, &ok);
  return {keep_running, ok};
}

}  // namespace example
