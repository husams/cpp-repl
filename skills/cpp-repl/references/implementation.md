# Debug cpp-repl itself

Locate the actual checkout/build before using these repository-relative paths.
Use this guide when the symptom belongs to the REPL implementation rather than
the user's C++ program. Preserve unrelated working-tree changes.

## Capture a reproducible boundary

Record the exact ordered inputs, stdout/stderr, exit signal, LLVM startup banner,
platform/architecture, launch directory, chosen database, startup inputs and
loaded-library paths. Compare the installed binary with the intended build.
Reduce separately: startup-only, C++ execution, `%add`, `%lib`, completion, hook
installation and calling the hook. Completion needs a real terminal/PTY; piping
a tab character does not reproduce replxx's Tab callback.

For a native crash on macOS, run the same input under LLDB (substitute the actual
binary), then capture the stopped stack:

```sh
lldb -- /absolute/path/to/cpp-repl
```

At the debugger prompt use `run`, reproduce the inputs, then
`thread backtrace all` and `image list`. For a piped-only reproduction, save its
input first and use LLDB's `process launch -i /absolute/path/to/input.repl`.
On Linux, use GDB or LLDB and collect all-thread backtraces plus shared libraries.
Native library initializers execute at load time; a loader-time crash is distinct
from an undefined-symbol diagnostic or a crash calling the function.

Use a debug build with a matching LLVM/Clang ABI when symbols are insufficient.
Do not replace the installed binary merely to collect evidence. A
`MonitoringException`/duplicate-registration message, unresolved symbol, or
completion diagnostic is a clue; the failing stack/input is needed to attribute
it to the loader, compiler, JIT, startup scripts or REPL recovery logic.

## Source map

| Boundary | Owning source / useful checks |
| --- | --- |
| Startup, pipes, exit status/history | `src/main.cpp`, `src/startup_files.cpp` |
| Interactive keys, Tab, multiline Enter | `src/repl.cpp`, `src/cpp_lexer.cpp`; `tests/pty_smoke.py` |
| `%` grammar, quoting and help | `src/commands.cpp`, `src/commands.hpp`; `tests/commands_test.cpp` |
| Shell substitution | `src/shell_substitution.cpp` |
| Compiler/AST/JIT execution and completion | `src/cpp_interpreter.cpp`; `tests/cpp_interpreter_test.cpp` |
| Failure cleanup and retained lookup | `src/interpreter_undo.hpp`, `src/cpp_interpreter.cpp`; `examples/include-recovery`, `examples/undo-crash` |
| Compile-command selection/options | `src/project_compilation_database.cpp`, `src/source_compile_options.cpp`; `tests/compilation_database_test.cpp` |
| Versioned definitions and file reload | `src/definition_shadowing.cpp`, `src/source_reload.cpp`; `tests/iterative_development_test.cpp` |
| Record/member inspection | `src/cpp_inspect.cpp`, `src/session_commands.cpp` |
| Symbol metadata and imports | `src/cpp_symbols.cpp`, `src/loaded_symbols.cpp`, `src/runtime_inspection.cpp` |
| Library loading and dependencies | `src/library_manager.cpp`, `src/library_dependencies.cpp`; `tests/library_dependency_test.cpp` |
| Missing native definitions/exports | `src/native_symbol_exports.cpp`, `src/interpreter_symbols.cpp`, `src/cpp_interpreter.cpp` |
| Hook routing and native caller patches | `src/function_hooks.cpp`, `src/native_call_patch*.cpp`, `src/native_runtime.cpp` |

Paths are a navigation aid; verify them against the target revision. Follow the
failing boundary rather than reading the whole tree. A no-match symbol listing
does not by itself establish a loader failure or an absent AST declaration.

## Verify a fix

The repository's macOS/Homebrew presets are:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Linux uses the `linux` presets with a matching LLVM/Clang development install;
configure `LLVM_DIR` and, when needed, `Clang_DIR` for that installation. The
README is authoritative for prerequisites and current platform limitations.
Do not mix a host compiler/library ABI with a different LLVM installation.

List tests with `ctest --test-dir build -N` and run the relevant existing test or
reproducer first. Add a behavioral regression for a code fix, checking the
failure and subsequent usable state, not just absence of a crash. Run the
appropriate broader suite after changing shared parser/JIT/recovery logic.
Do not report historical test counts or another platform's results as current
verification. Native-hook fixes need evidence at the actual affected call site;
a successful JIT prompt call alone does not validate a native caller patch.
