# Shell commands and screen clearing

Worktree: `codex/shell-substitution`, based on `408976e`.

- [x] Add deferred double-backtick shell substitution to all percent commands.
- [x] Support multiline editing and startup scripts; preserve C++ blocks.
- [x] Capture stdout once; report process failures without running the command.
- [x] Add `%clear`, completion/help entry, and usage validation.
- [x] Document syntax, quoting, output and process behavior.
- [x] Complete unit, terminal, piped-input and startup-script validation.

Code exploration used the CTK Python SDK with this worktree's compilation
database to inspect definitions and calls in commands.cpp, repl.cpp and
session_commands.cpp. The wiki index had no page for these command paths.

Validation (macOS ARM64, LLVM 22.1.8): build succeeded; all 186 CTest tests
passed, including terminal editing/cancellation, external script loading,
compiler-option substitution, quoted paths, piped stdin isolation, separate
stderr, startup scripts and `%clear` preserving C++ state. Unit checks also
cover nonzero/signal exits, NUL output rejection, output larger than a pipe
buffer, multiple substitutions, trailing newlines and literal C++ backticks.
The new process module passes clang-format; `git diff --check` is clean.
