# Type and shell commands

Base: origin/main at 197c9a8 (latest editor hints and session options).
The five existing uncommitted portability/example changes were copied into this
isolated worktree; the original checkout remains untouched.

- [x] Ground routing, AST declaration lookup and shell spawning with CTK.
- [x] Implement `%type` with `(type)` and `(parameters) -> result` output.
- [x] Implement single-line `!` commands with streamed stdout/stderr.
- [x] Validate regression tests, examples and editor behavior.

Grounding: `scripts/type-shell-grounding.ctk` ran through CTK's disposable
server with this worktree's compilation database. It found `parse_command`,
`execute_command`, `describe_symbols`, zero Interpreter execution calls in
the declaration lookup and the existing `posix_spawn` call.

## Delivered

Branch: `codex/type-shell-commands`.
Binary: `/Users/husam/.codex/worktrees/type-shell-commands/replxx-example/build/cpp-repl`.
The PR contains only the two commands and their documentation/tests. The five
copied portability/example changes remain local and uncommitted. No install
was performed.

- `%type x` prints `(int)` for `int x = 0;`.
- `%type f` prints `(int) -> int` for `int f(int x)`.
- Qualified symbols and typed overload selectors reuse AST lookup, without
  evaluating expressions or looking up JIT addresses. Bare names list overloads.
- `! pwd` and `!pwd` run one shell line; stdout/stderr stream to the REPL.
  Output before an unsuccessful exit is preserved. Child stdin is `/dev/null`.
- Help, symbol completion, REPL colors, literal shell typing and Vim syntax
  support both commands. README includes usage and shell behavior.

## Verification — 2026-10-26

- macOS ARM64 / LLVM 22.1.8: **318/318 CTest tests pass**, including the
  expanded PTY smoke test. `build/type-shell-ctest.log` contains the full run.
- linux-dev x86_64 / LLVM 21: **315/316 pass**; every new test and the expanded
  PTY smoke test pass. `build/type-shell-linux-ctest.log` contains the run.
- Linux's `IncludeRecovery.FailedAddPreservesNewSystemHeaderNamespaces`
  terminates with SIGILL after the unresolved-symbol diagnostic. A separate
  build of unchanged `origin/main` (197c9a8) with the same pre-existing local
  portability fixes reproduces the same failure. This is a baseline issue;
  `build/type-shell-linux-baseline.log` records the isolated comparison.
- The CTK script ran successfully against the final implementation: it found
  `CppInterpreter::symbol_type` and zero Interpreter Parse/Execute calls there.
- Vim syntax checks and `git diff --check` pass.
- Review caught a single-line parsing edge case: newline whitespace immediately
  after `!` could be skipped. Parsing now skips only spaces/tabs there, and
  regression cases reject both LF and CRLF before the shell command.
- Independent Sol High review approved the final change with no remaining
  actionable findings. After the fix, all five focused tests pass on macOS and
  Linux; all 57 command tests and the Vim syntax checks also pass on macOS.
- A scratch REPL session printed exactly `(int)` and `(int) -> int`, then its
  current directory and shell output; a subsequent `f(42)` still printed 42.
