# Overload selection menu

Worktree: `codex/editor-committed-hints`, based on `b77bba1`.
The primary checkout and its unrelated changes remain untouched.

- [x] Reproduce the missing selection workflow in a real terminal.
- [x] Add a signature menu with Up/Down selection and Tab acceptance.
- [x] Preserve the chosen identity for parameter hints and argument navigation.
- [x] Verify cancellation, ordinary editing, history and non-call completion.
- [x] Ground the final paths with CTK and obtain independent review.
- [ ] Run complete macOS and Linux suites and verify the installed executable.

Selection guides the editor's hints and argument slots. Clang continues to resolve
the actual C++ call from the supplied arguments; browsing never executes code.
