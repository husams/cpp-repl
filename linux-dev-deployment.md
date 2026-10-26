# Linux dev deployment

Host: `linux-dev`, user `husam`, x86-64, LLVM/Clang 21.1.8.
Source: `4b21a9dd19bc044cc2bd6e9b213406beb0a1b8de` on `codex/editor-committed-hints`, including overload selection,
session options, large-session performance fix and LLVM 21 compatibility guards.

Release directory:
`/home/husam/.local/share/cpp-repl/releases/20261026-editor-verified`.
Installed command: `/home/husam/.local/bin/cpp-repl`.
The previous installed executable is retained as `previous-cpp-repl` in the
release directory. Remote source and build files are isolated from existing
checkouts; unrelated changes in the primary macOS checkout remain untouched.

Validation:

- Linux full suite: 311/311 passed in 177.98 seconds, including real-terminal
  large-header completion, semantic coloring, inline hints and argument navigation.
- macOS full suite: 313/313 passed in 163.19 seconds.
- Paired large-context benchmarks passed all four macOS/Linux comparisons at
  30,000 and 100,000 declarations with native libraries and standard headers loaded;
  see [measurements and tolerances](editor-large-session-performance.md).
- Installed-release smoke: LLVM 21.1.8 banner, both options defaulting to true,
  a semicolon-terminated call printing 30, and a muted call still executing once.
- Option, committed-hint and overload-menu terminal suites passed against the installed
  executable, including blank slots, clean history and final gray annotations.
  Embedded, nested and multiple calls keep parameter names/types; return
  annotations are omitted when expression code follows the call. Methods,
  earlier-argument cursor edits, comments and strings are covered as well.
- Overload signatures support Up/Down selection and Tab acceptance for global,
  method and explicit-template calls. Selection drives parameter hints and Tab
  stops; C++ resolves execution normally. Cancellation, bounded menu rows,
  ordinary history and command-specific completion are covered.
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

The release directory retains configure/build/install logs, `tests.log`,
`macos-tests.log`, CTK grounding, independent review, source revision metadata,
binary checksums and smoke output. The earlier `20261026-expression-hints` release
also recorded unresolved vector/path destructors in a broader standard-library
temporary-object probe, including with the vendor driver config; that existing
LLVM 21 execution limitation is separate from these editor changes.
