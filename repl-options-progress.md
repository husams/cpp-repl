# Expression output and committed hints

Worktree branch: `codex/editor-committed-hints`, based on `cb62cdd`.
The primary checkout and unrelated changes remain untouched.

- [x] Reproduce disappearing annotations on Enter with a real-terminal test.
- [x] Retain completed-call annotations on the final repaint, even after `;`.
- [x] Add strict `%set option = true|false` and `$option` query grammar.
- [x] Replace visible placeholder markers with blank argument slots.
- [x] Capture top-level scalar expression values through Clang, including semicolons.
- [x] Verify both runtime options, completion, independent defaults and errors.
- [x] Run complete macOS regression tests and review the final implementation.
- [ ] Build, verify and redeploy on `linux-dev`.

`print_values=true` prints values for non-void expressions, with or without a
semicolon. `print_values=false` mutes automatic value output; expressions still
execute once. Declarations, control flow, source-file contents and explicit
program output retain their meanings. The library `evaluate()` API is separate.
Semicolon expressions with nontrivial temporary cleanup keep ordinary C++
evaluation without automatic value capture, avoiding a Clang cleanup limitation.

`keep_hints=true` retains gray parameter names, types and return annotations on
submitted calls. `keep_hints=false` clears them from the committed terminal line;
editing hints remain available. Both options default to true and are per session;
startup `.repl` files can use the same `%set` commands.

Final macOS suite: 312/312 passed. CTK grounding completed successfully and an
independent review approved the final implementation. Regression tests cover
recovery after invalid expressions and exact destructor counts for class results,
scalar member calls and function arguments containing nontrivial temporaries.
The terminal latency harness drains pending output while awaiting shutdown.
