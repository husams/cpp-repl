# Statement keyword completion — 2026-10-27

Branch: `codex/return-keyword-completion`, based on main `b252ad8`.

- Reproduced missing `return` completion in a failing regression test.
- The completion consumer discarded Clang `RK_Pattern` results outside
  preprocessor contexts. Statement keywords such as `return` use this result kind.
- Preserve their typed keyword text without snippet placeholders or call insertion.
- Added coverage for integer/void functions, multiline input, lambdas, `for`,
  `while`, and `if`; completion leaves the live interpreter usable.
- The terminal regression types `ret` then Tab inside a multiline function,
  finishes its body, and verifies calling it returns 42.
- CTK's editor-grounding script ran against this worktree's compilation database.
- Independent review approved the fix with no actionable findings.
- Focused editor-context and terminal tests pass on macOS; Linux keyword cases pass.
- Full macOS suite: **319/319 pass**, including the terminal regression.
- Full Linux suite: **316/317 pass**, including the keyword and terminal tests.
  The only failure is the already confirmed baseline
  `IncludeRecovery.FailedAddPreservesNewSystemHeaderNamespaces` SIGILL.
- Full logs are `build/return-completion-ctest.log` and
  `build/return-completion-linux.log`; `git diff --check` passes.

The five copied portability/example changes are excluded from the fix commit;
the original checkout's edits are preserved. No installation was performed.

Test executable:
`/Users/husam/.codex/worktrees/return-completion/replxx-example/build/cpp-repl`
