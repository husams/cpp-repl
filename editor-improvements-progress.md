# Embedded Clang editor improvements

Scope: richer completion (types, overloads, kinds, ranking, parameter hints) and
semantic identifier colors, with lexical fallback. No LSP. All edits and builds
are in the `codex/embedded-clang-editor` worktree; the primary checkout's local
changes are excluded.

- [x] CTK AST grounding: `CppInterpreter::complete` calls `make_compiler`,
  `ReplCodeCompleter::codeComplete`, and `member_names`; `highlight_cpp` calls
  `lex_cpp` and `token_color`. Existing completion results hold names/prefix only.
- [x] Add behavioral tests and structured editor results.
- [x] Implement syntax-only analysis importing current session declarations.
- [x] Present details separately from inserted completion text.
- [x] Overlay semantic colors and preserve command/embedded-language boundaries.
- [x] Validate completion, session isolation, terminal behavior, and full suite.

Grounding endpoint: private local CTK server; compilation database generated in
this worktree's build. LLVM reference: llvmorg-22.1.8
`clang/lib/Interpreter/CodeCompletion.cpp` and installed public Clang headers.

Implemented behavior:

- Completion rows keep overload signatures, parameter names, return/data types,
  semantic kinds and Clang priorities separately from identifier insertion.
- Signature hints report the current argument. Access, constness, local shadowing,
  inheritance, calls, namespaces, using directives and current redefinitions use
  Clang's semantic lookup.
- A disposable syntax-only compiler imports the session AST and macro state.
  Editor requests never run the edited input or commit its declarations/macros.
- Semantic spans distinguish declarations and references, including locals,
  parameters, fields, methods, types, namespaces, enum constants and macros.
  UTF-8 offsets are converted for terminal colors; lexical/command colors remain
  authoritative for strings, comments, escapes, paths and shell blocks.
- Repeated buffers reuse cached results until the incremental TU or flags change.
  Unresolved/incomplete constructs and buffers above 64 KiB retain lexical colors.

Review regressions addressed:

- Import namespace shells lazily to avoid eagerly importing all standard-library
  declarations; complete instantiated types when they are actually used.
- Guard recursive namespace lookups (`std::cout` exposed a stack overflow).
- Normalize the REPL end-of-input annotation to ordinary EOF for syntax-only
  analysis so unfinished function bodies cannot spin in Clang recovery.
- Preserve command-line macro undefinition/redefinition, variadic macros and token
  pasting. Suppress import diagnostics without poisoning later execution.
- Keep obsolete version namespaces out of current lookup while retaining old
  object types; use Sema lookup to respect locals shadowing globals.
- Reuse reopened anonymous namespaces, import their implicit using directives and
  skip empty-name lookup when determining whether their members are current.
- Extend pinned replxx display labels without changing inserted identifiers;
  signature hints cannot be selected as completion text.

Validation (macOS ARM64, Homebrew LLVM 22.1.8):

- Full build passed without compiler warnings. Final suite: **257/257 passed**
  in 123.55 seconds, including both final namespace regressions.
- Both terminal regressions passed: the existing PTY smoke test and
  the new focused editor PTY test (overload menu, clean insertion, hints, ANSI
  semantic colors, cancellation and execution).
- Local build/test evidence: `build/editor-build.log`,
  `build/editor-final-tests.log`, and `build/editor-grounding.log`.
- The complete replxx patch applies to a clean pinned checkout and reapplication
  is idempotent. New C++ files pass clang-format; `git diff --check` passes.
- CTK script rerun after implementation: two editor-analysis calls, zero direct
  Interpreter execution calls, syntax-only ExecuteAction and the lexical/semantic
  overlay path confirmed. Reproduce with:

  ```sh
  ctk --server unix:///tmp/cpp-repl-editor-ctk.b1H303/server.sock \
    --compile-commands build/compile_commands.json \
    --script scripts/editor-grounding.ctk root="$PWD"
  ```

Current limits: analysis is synchronous, so header-heavy buffers or large sessions
can delay a repaint; only macOS/LLVM 22 has been exercised here. Documentation is
updated in README.md. No LSP, commits to the primary checkout, or remote publishing.

## 2026-10-26 — contextual preprocessor completion correction

The reported `#`/Tab bug was reproduced with a failing regression: directive
patterns were discarded, then the ordinary session-symbol supplement added C++
types and variables to a preprocessor request.

- Retain Clang's typed directive-pattern text without inserting placeholders.
- Respect directive, macro-name, conditional-expression and include-file contexts;
  do not add ordinary session declarations to preprocessor choices.
- Complete active macro operands from the disposable compiler's preprocessor,
  including compiler macros such as `__cplusplus`.
- Keep generic C++ pattern handling unchanged to avoid misclassifying template
  snippets as keyword suggestions.
- Clang's observed `#i` choices are `if`, `ifdef`, `ifndef`, `include` and
  `include_next`; it is not a unique prefix. A unique prefix such as `#def`
  inserts `define` immediately.
- Added coverage for `#`, `#i`, indentation/multiline input, macro definitions,
  `#ifdef`/`#if` operands, compiler macros and colliding C++ symbol prefixes.
- The enhanced editor PTY regression passed: directive-only menu, prefix
  filtering and execution of a macro defined through a completed `#def`.
- CTK grounding rerun confirms the completion path reads Clang's context and
  typed pattern text before Sema lookup; evidence in
  `build/editor-directive-grounding.log`.

Full build passed without warnings; **258/258 tests passed** in 125.20 seconds,
including the enhanced editor PTY test. Evidence: `build/editor-directive-build.log`
and `build/editor-directive-tests.log`.

## 2026-10-26 — Tab after moving into a field initializer

Reproduced the reported hang using `struct C { int x = 1; int y = 0;  };`
with the cursor immediately after the first `=`. Replxx correctly passes only
the text before the cursor, so Clang analyzes the incomplete prefix
`struct C { int x =`. Before the fix, that request exceeded a 10-second deadline.

The disposable editor compiler now converts the REPL input-end annotation to
ordinary EOF for completion requests as well as semantic coloring. Clang's
code-completion token is preserved. This prevents parser recovery from looping
after an unfinished initializer; it does not affect execution in the persistent
interpreter.

- A child-process regression bounds completion to 10 seconds, covers two field
  initializers, a function-body initializer and a nested-namespace initializer,
  and verifies subsequent arithmetic. The fixed request completes in milliseconds.
- The editor PTY test types the supplied complete struct, moves the cursor back
  after `=`, presses Tab, cancels editing and evaluates arithmetic successfully.
- Empty-prefix completion can still show replxx's `--More--` pager. Space/Enter
  continue it, `q` dismisses it, and Ctrl+C dismisses the menu; a second Ctrl+C
  cancels the edited line. This is distinct from the fixed parser loop.
- Final core/editor regression run: **258/258 passed** in 20.96 seconds;
  `build/editor-cursor-tests.log`. The original terminal smoke regression also
  passed in 122.71 seconds, alongside a fresh pass of the bounded initializer
  test (`build/editor-cursor-final.log`): **all 259 distinct tests passed**.
- CTK grounding rerun against the corrected analysis path; evidence in
  `build/editor-cursor-grounding.log`.

## 2026-10-26 — context audit and paired input

Requested behavior: complete functions/methods with `()` (no inserted argument
commas), keep the cursor inside and show their signatures. Automatically close
parentheses, braces and quotes while preserving existing closing characters.

- [x] Add include-path, member-access and unfinished-input context regressions.
- [x] Propagate include-name replacement spans, including directory components
  and partial filename extensions, from Clang to the terminal editor.
- [x] Validate callable insertion, selection cycling and reuse of existing calls.
- [x] Validate paired input, escaping, empty-pair Backspace and command boundaries.
- [x] Run terminal regressions and complete the context audit.

The new terminal interaction test exercises free functions, methods through `.`,
`->` and call results, signature hints, zero-argument calls, mid-name replacement,
cycling choices, UTF-8, comments, quoted nested includes and angle includes.

Audit findings and completed corrections:

- Include completion normalizes Clang's basename/full-path candidates and closing
  delimiters, replaces complete path prefixes and suffixes, and permits real
  header names beginning with `__`. Tests use private `-I`/`-isystem` fixtures.
- Comments receive no C++ symbol suggestions. Session-name supplementation is
  limited to ordinary expression/statement/name contexts, so an alias's type-only
  position does not receive variables. Member tests cover inheritance, access,
  const receivers, pointers, call results and function signature metadata.
- Callable completion retains separate display signatures and adds `()` only to
  inserted call text. Cursor offsets handle unique completion, forward/reverse
  selection, existing parentheses and mid-identifier edits. Cycling back restores
  the original remaining suffix even after extending a shared prefix.
- Replxx now renders signature hints when the cursor is inside a paired call,
  including calls in embedded C++ command bodies, and refreshes them on movement.
- Pairing applies to explicitly typed C++ keys, including command payloads;
  comments, escapes, raw prefixes, numeric apostrophes and shell/path operands
  retain their syntax. Quote handling accounts for escape-separated lexer tokens.
- Enter preserves body/parameter continuation and valid completed calls; command
  operands keep command grammar semantics. The earlier smoke test's completed
  two-argument call was updated to use the newly inserted opening parenthesis.
- Empty-pair Backspace is checked by deleting a pair and then successfully
  entering a declaration, rather than by evaluating an equivalently grouped sum.

Validation: **274/274 tests passed** in 123.30 seconds, including the existing
terminal smoke and both editor terminal tests (`build/editor-pairs-final.log`).
The final focused checks additionally cover internal-header names and the stronger
Backspace case (`build/editor-pairs-last-check.log`). Build output is in
`build/editor-pairs-build.log`; no compiler warnings were emitted. New C++ files
pass clang-format and `git diff --check` passes. The expanded replxx patch applies
cleanly to a fresh pinned checkout and its second application is idempotent.
The extended CTK script verifies the embedded analysis, context, pairing and
frontend hint/edit paths (`build/editor-pairs-grounding.log`).
