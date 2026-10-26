# Overload selection menu

Worktree: `codex/editor-committed-hints`, based on `b77bba1`.
The primary checkout and its unrelated changes remain untouched.

- [x] Reproduce the missing selection workflow in a real terminal.
- [x] Add a signature menu with Up/Down selection and Tab acceptance.
- [x] Preserve the chosen identity for parameter hints and argument navigation.
- [x] Verify cancellation, ordinary editing, history and non-call completion.
- [x] Ground the final paths with CTK and obtain independent review.
- [x] Run complete macOS and Linux suites and verify the installed executable.

Selection guides the editor's hints and argument slots. Clang continues to resolve
the actual C++ call from the supplied arguments; browsing never executes code.

Implemented in `8f17c3c` and `b155db3`. The final CTK script confirms the Clang
overload metadata path, display callback, command-context guard and absence of
direct execution calls in the frontend's editor paths. Independent review approved
the implementation and the template/command-context regression fixes.

Final full suites passed: macOS 313/313 (163.78 seconds), Linux 311/311
(143.77 seconds). The Linux release is isolated under
`~/.local/share/cpp-repl/releases/20261026-overload-menu`; its committed source
was verified before installation, and the installed executable matches the staged
binary. Option, committed-hint and overload-menu terminal tests all passed against
`~/.local/bin/cpp-repl`. The previous executable remains in the release directory.
