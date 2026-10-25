---
name: cpp-repl
description: Use cpp-repl, the Clang Interpreter C++ REPL, to reproduce bugs, investigate C++ behavior, inspect classes and native symbols, load project sources and libraries, and test behavior with mocks and pre/post hooks. Applies to cpp-repl usage and debugging its implementation; use the actual target debugger for another running process.
---

# Investigate with cpp-repl

Use small executable experiments to explain observed C++ behavior. Distinguish
what the compiler knows, what the native loader resolves, and what actually runs.
This is the `husams/cpp-repl` tool (repository sometimes named `replxx-example`),
not stock `clang-repl`, CTK, or a general-purpose C++ MCP server.

## Establish the session

- Resolve `cpp-repl` with `command -v cpp-repl`, or use an explicit repository
  build such as `./build/cpp-repl` or `./build-linux/cpp-repl`. Keep the binary's
  absolute path when changing directories. Do not assume it matches the checkout.
- Record the startup LLVM version, working directory, arguments and `%help`.
  Startup arguments are **compiler flags**; do not invent application flags such
  as `--no-startup`, `--version`, or `--script`.
- Check relevant startup scripts before reproducing: numbered `<n>-<name>.repl`
  files in `~/.cpp-repl/`, followed by the nearest project `.cpp-repl/` directory.
  Both can run C++, load libraries and alter flags. A temporary working directory
  excludes project configuration, but still loads user scripts. Do not move,
  edit or execute unrelated user scripts merely to obtain a clean experiment.
- The nearest discovered `compile_commands.json` (including `build/` and
  `build-linux/`) is selected at startup. Record the selected path. Database and
  startup-script discovery depend on launch directory, not the skill directory.
- Use the target's `%help` and matching README for capability checks. A newer
  checkout can document `%methods`, `%fields`, or `%define` subfeatures absent
  from an installed binary; test the needed subfeature before relying on it.

Run a small reproduction with a **quoted** shell heredoc so the shell cannot
execute the REPL's backticks. Ordinary piped C++ must fit one physical line per
input; `%mock`/`%pre`/`%post`/`%define` blocks can span lines. Use `%add` for
multiline C++ files or a terminal session for interactive editing/completion.

```sh
cpp-repl <<'REPL'
int twice(int x) { return x * 2; }
twice(21)
%syminfo twice
%quit
REPL
```

Expected value: `(int) 42`. Save stdout **and** stderr for a bug report.
An ordinary input error can still end with process exit status zero; check the
diagnostics and expected result, not just the exit code.
The REPL writes `.replxx_history` in its working directory even with piped input.
Use an owned scratch directory for isolated experiments; preserve existing
project history when the project's launch directory is needed.

## Select the smallest useful observation

| Question | Commands / action |
| --- | --- |
| What settings compile this file? | `%compile-opts path.cpp`; `%use-compile-opts path.cpp` to apply its settings to later prompt input |
| What source is in this session? | `%add` lists it; `%add path.cpp` parses and executes it |
| What classes or layouts are available? | `%class`, then `%class ns::Type` |
| What are a record's private or inherited members? | If listed in `%help`: `%methods ns::Type access private include-parents true` and `%fields ns::Type access all include-parents true` |
| Which native libraries are loaded? | `%lib` lists explicitly loaded libraries; `%libs [all] [regex]` includes dependencies |
| Why did library lookup fail? | `%paths`, `%libinfo library`, `%add-path /absolute/libdir` |
| Is a symbol defined, resolved, or referenced? | `%syminfo ns::func` (`%info` is an alias); `%demangle _Z3addii` |
| Which names or imports match? | `%symbols lib 'libname' name '^ns::'`; `%symbols undefined`; add `all` for system images |
| Is instrumentation changing the result? | `%hooks`, an exact `%restore signature`, then rerun the same call |
| Did edited source take effect? | If supported, `%reload path.cpp`; compare newly created objects/callers with old ones |

Start symbol inspection with a bare qualified name and read its declaration or
overload set. Some binaries reject full-signature `%syminfo` queries even when
the function exists. On such a miss, retry the bare name; do not infer a missing
definition. Hook/restore commands still require the exact target signature;
their signature support does not establish signature support for symbol queries.

Load library declarations with a header (`#include` or `%add`) as well as loading
the binary with `%lib`. Include paths (`-I`) and native library search paths
(`%add-path`) solve different problems. A header alone does not define a missing
out-of-line function or static data member.

For missing imports, native-library inspection, and controlled hooks, read
[the investigation recipes](references/investigation.md). For a crash or a bug
in cpp-repl itself, read [the implementation guide](references/implementation.md).

## Interpret results correctly

- Inputs share one persistent AST and execute native code in this process.
  An expression without a final `;` prints its value. Native crashes terminate
  the process; external side effects of executed C++ cannot be rolled back.
- `%class` and symbol queries inspect retained declarations/metadata; they do
  not call the inspected function. Member access in an executable expression
  still obeys ordinary C++ access rules.
- A successful include retains header guards and compiler state. Failure
  recovery is not a complete compiler transaction. Include needed headers in a
  successful earlier input. After an error, test an earlier declaration as well
  as arithmetic: some installed builds lose source lookup for a namespace
  reopened by rejected input while old compiled callers still run. If that
  happens, replay the known-good setup in a fresh process; a newer checkout's
  reported fix is not proof that the installed binary contains it.
- In versions supporting redefinition/reload, later inputs use the newest
  definitions, while old objects, pointers and compiled callers can retain old
  types/code. Reload can rerun initializers; flag edits affect future compilation.
- `%clear` only clears the display. `%restore` removes hooks; it does not reset
  declarations, objects, loaded libraries or external effects. A fresh process
  is the reliable baseline. Do not invent a `%reset` or `%undo` command.
- Symbol/reference listings are partial. Missing output can mean a hidden,
  stripped, filtered, unemitted or unsupported symbol; zero known references
  is not proof that no callers exist. Addresses are session-specific.
- `%lib` loads into this REPL process. It does not attach to an existing server
  or reveal that server's heap/state. Reproduction here is evidence about this
  experiment; use the target process's debugger for its actual runtime state.
- Double-backtick blocks in `%` command arguments execute `/bin/sh -c`;
  ordinary `cpp` backtick blocks contain C++. Avoid shell substitution when
  literal arguments suffice, and never interpolate untrusted content into it.

Report the observed result, minimal ordered reproduction, binary/LLVM identity,
relevant compile settings and the evidence supporting the explanation. Label
untested hypotheses and native coverage limits; do not infer a diagnosis from
an error substring alone. Match the user's requested level of detail.
