# Compile-option commands

- [x] Cover command grammar, quoting, scope, replacement, precedence, and recovery.
- [x] Add inspection, append, and replacement commands for global and file scopes.
- [x] Apply global updates to future interactive inputs, hooks, and file additions.
- [x] Preserve live session state and validate updates before storing them.
- [x] Document syntax and verify on macOS and Linux.

Commands: `%compile-opts [file]`, `%add-compile-opt [file] <flags...>`, and
`%set-compile-opts [file] [flags...]`. Optional filenames precede flags, with
shell-style quoting and escaping and no shell expansion or separator argument.
Global replacement replaces startup/previous global flags. File replacement
replaces database and previous file flags while retaining globals. Appends
preserve their existing scope. An empty replacement clears that scope.

Effective order is database, globals, then file overrides, followed by mandatory
platform/JIT settings. Manual relative include paths are anchored when entered.
Global changes affect subsequent C++ input and hook compilation, and all flags
affect future file additions. Existing values, declarations, and hooks are
preserved. Inspection and configuration do not run forced-include initializers.
Runtime changes configure preprocessing/language settings; JIT target and code
generation stay session-wide, as documented in the option report and README.

Validation uses Clang's driver option table with Clang visibility (avoiding
clang-cl aliases such as `/O2`), and a disposable frontend to validate native
preprocessor definitions and macro headers. Invalid updates preserve previous
settings; per-input macro/language/header-search scopes recover after errors.

Verification: full suites passed 146/146 on macOS ARM64 (LLVM 22.1.8) and
144/144 on linux-dev x86_64 (LLVM 21.1.8), including terminal quoting, filename
completion, help, inspection, precedence, rejected updates, and clearing.
Final focused tests passed 16/16 on each platform after adding optimized
compilation-database response-file coverage. The disabled-native Linux build
passed 20/20 focused tests, including optimized hooks and library dependencies.
Manual macOS use also verified C++20 `char8_t`, string-valued macro flags,
global replacement, append, inspection, and clearing.

## File references and startup preloading

- [x] Add `%use-compile-opts <file>` with quoted paths and completion.
- [x] Adopt database/file flags for future session input without compiling the reference.
- [x] Preserve relative build paths, live state, manual globals, and atomic failure behavior.
- [x] Rebase settings when switching references; retain append overrides and clear metadata on replacement.
- [x] Preload all source/header paths listed in `~/.cpp-repl` in order, using `%add` options.
- [x] Continue after invalid/missing entries, skip duplicates, and ignore an absent list.
- [x] Validate unit and terminal behavior on macOS and Linux.

Full verification passed 152/152 tests on macOS and 150/150 on linux-dev,
including interactive reference selection and default `~/.cpp-repl` loading
before the first prompt. Final focused checks passed 11/11 on each platform;
the disabled-native Linux configuration passed 26/26 checks. The reference
regressions verify relative include directories, retained manual globals,
switching standards, unchanged live values/hooks, snapshots, and rejected
updates. Startup regressions cover file order, quoted/literal paths with
spaces, errors followed by valid entries, duplicates, absent lists, and
automatic compilation-database options. Compilation-command source operands
after `--` are stripped before copying flags into session defaults.

## Ordered startup commands

- [x] Execute `%commands` and source/header entries in configuration order.
- [x] Share command execution with the interactive REPL, including library loading.
- [x] Apply reference defaults before subsequent files; preserve existing bare paths.
- [x] Preserve multiline hook bodies, report configuration line numbers, and honor `%quit`.
- [x] Document command path resolution and the ordered configuration example.
- [x] Verify unit and terminal behavior on macOS and linux-dev.

Full suites passed 157/157 on macOS ARM64 (LLVM 22.1.8) and 155/155 on
linux-dev x86_64 (LLVM 21.1.8). Coverage includes database reference defaults
before later files, preserved interactive defaults, command errors and line
numbers, multiline hooks with blank lines/directives, restore, library calls
from preloaded files, and startup quit before the first prompt. Formatting
checks for the new execution/startup code and `git diff --check` also passed.

## Startup directory correction

The startup-list-file design above is superseded: `~/.cpp-repl/` is a directory.

- [x] Load `<number>-<name>.repl` scripts in numeric order, with filename tie breaks.
- [x] Accept C++ and commands in scripts, including multiline inputs and blank lines.
- [x] Use `%add` for source/header loading after session-default setup scripts.
- [x] Ignore other names and nested directories; report a regular file at the directory path.
- [x] Recover after errors and incomplete script inputs; stop all startup work on `%quit`.
- [x] Verify directory startup through the terminal on macOS and linux-dev.

Focused startup and complete terminal smoke coverage passed 9/9 checks on
macOS ARM64 and 9/9 on linux-dev x86_64. Tests verify numeric ordering (including
leading zeros, ties, and large order numbers), ignored unrelated/nested files,
database defaults and extra flags before later scripts, multiline C++/hooks,
library loading, error recovery, incomplete script boundaries, and `%quit`.
Source formatting and `git diff --check` passed.
