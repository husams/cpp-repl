# Linux dev deployment

Host: `linux-dev`, user `husam`, x86-64, LLVM/Clang 21.1.8.
Source: `d3901d3` on `codex/editor-committed-hints`, including the earlier
session options, large-session performance fix and LLVM 21 compatibility guards.

Release directory:
`/home/husam/.local/share/cpp-repl/releases/20261026-expression-hints`.
Installed command: `/home/husam/.local/bin/cpp-repl`.
The previous installed executable is retained as `previous-cpp-repl` in the
release directory. Remote source and build files are isolated from existing
checkouts; unrelated changes in the primary macOS checkout remain untouched.

Validation:

- Linux full suite: 310/310 passed in 166.98 seconds, including real-terminal
  large-header completion, semantic coloring, inline hints and argument navigation.
- macOS full suite: 312/312 passed in 162.52 seconds.
- Installed-release smoke: LLVM 21.1.8 banner, both options defaulting to true,
  a semicolon-terminated call printing 30, and a muted call still executing once.
- Both option and committed-hint terminal suites passed against the installed
  executable, including blank slots, clean history and final gray annotations.
  Embedded, nested and multiple calls keep parameter names/types; return
  annotations are omitted when expression code follows the call. Methods,
  earlier-argument cursor edits, comments and strings are covered as well.
- Dynamic dependencies resolve without missing libraries.
- Installed and staged executables have identical SHA-256 checksums; source
  files were verified against the committed source before installation.
- CTK grounding and independent review passed.

Both `%set print_values = false` and `%set keep_hints = false` are supported;
`$print_values` and `$keep_hints` show their current settings. Semicolon
expressions requiring nontrivial temporary cleanup execute normally without
automatic value output, preserving C++ destruction semantics despite a Clang
value-capture limitation.

Run from the Mac:

```sh
ssh -t linux-dev /home/husam/.local/bin/cpp-repl
```

Or, while logged into Linux:

```sh
~/.local/bin/cpp-repl
```

The release directory retains configure/build/install logs, `tests.log`, source
revision metadata, binary checksums and smoke output. A broader standard-library
temporary-object probe reported unresolved vector/path destructors in both the
previous installed build and this build, including with the vendor driver config;
that existing LLVM 21 execution limitation is separate from these editor changes.
