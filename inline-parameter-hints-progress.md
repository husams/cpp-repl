# Inline parameter hints

Worktree: `codex/inline-parameter-placeholders`, based on merged editor PR #13.
The primary checkout and its unrelated changes remain untouched.

- [x] Ground the existing completion, signature, and rendering paths with CTK.
- [x] Expose Clang parameter names/types/defaults and stable overload metadata.
- [x] Render gray parameter placeholders inside calls, with return type outside.
- [x] Navigate actual argument slots using Tab and Shift+Tab.
- [x] Keep decorations out of executable input and history.
- [x] Verify defaults, zero-argument methods, nested expressions, and UTF-8.
- [x] Run the full suite and independently review the implementation.

Labels are presentation only. The buffer begins as a normal `function()` call;
navigation materializes commas only as needed. Required empty argument slots
remain unfinished input, and optional arguments may be omitted.

Completion still handles unfinished identifiers inside argument slots; complete
arguments advance with Tab. Ctrl+N/Ctrl+P select overloads, and a selected outer
signature survives completion of nested calls. Selections follow source edits
and reset when the input is submitted or cancelled.

Verification uses macOS ARM64 with LLVM 22.1.8:

- Eight call-slot unit tests and structured Clang metadata tests pass.
- The real-terminal snippet test passes 50 assertions, including nested
  completion, selected overloads, default-argument omission after Tab,
  cancellation, UTF-8, ordinary commas and clean history.
- CTK confirms syntax-only analysis, Clang parameter metadata, lexical call
  slots, and the inline display/navigation callbacks.
- The tracked replxx patch applies and reverses cleanly on a fresh local clone,
  and exactly matches the dependency used by the build.
- Independent review reproduced and verified fixes for nested completion and
  selected outer-overload retention; the final targeted review found no remaining
  issue. Narrow-terminal wrapping and filled-slot cursor placement also passed.
- Final full suite: 297/297 passed in 159.88 seconds. The per-key echo latency
  regression also passed (median 2.6 ms, maximum 2.8 ms).

Try this worktree's executable with:

```sh
cd /Users/husam/.codex/worktrees/embedded-clang-editor/replxx-example
./build/cpp-repl
```

Detailed local evidence is in `build/inline-final-full-tests.log`,
`build/inline-hints-ctk-final.log` and `build/inline-parameter-review.md`.
