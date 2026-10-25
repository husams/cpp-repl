# Record member commands

Status: complete (2026-10-25).

Implemented in the isolated `codex/record-members` worktree, based on
`2a74837` from `origin/main`. The original checkout's ongoing edits are preserved.

- Added `%methods` and `%fields` for parsed classes, structs and unions.
- Defaults: `access all`, `include-parents false`; either option order is accepted.
- Clang AST matchers select members from the retained AST without parsing or
  executing inspection inputs. Output includes qualified names and types.
- Parent groups follow declaration order depth first, with each ancestor once.
  Filters use declared access, including for privately inherited bases.
- Static data members, method templates, constructors, destructors, overloads
  and operators are covered. Compiler-generated implicit members are omitted.
- Added help, highlighting, completion, documentation and the manual example
  under `examples/record-members/`.

## Verification

macOS ARM64, Homebrew LLVM/Clang 22.1.8:

- Fresh default configure/build: passed without compiler warnings.
- Full CTest: **223/223 passed**, including terminal smoke and completion checks.
- All seven existing example directories passed: `points`, `undo-crash`,
  `include-recovery`, `static-members`, `unresolved-library`,
  `unresolved-callables`, `unresolved-virtuals`.
- New `record-members` checked example: passed (eight example directories total).
- Points replay verified all seven return values, pre/post output and restore.
- New tests cover defaults, option validation, all access levels, aliases,
  private/diamond inheritance, static fields, overloads, templates, qualified
  function/array types, qualifiers, empty/unknown records and redefinition.
- Shell syntax and `git diff --check`: passed.
- Code review completed; no unresolved findings. Linux was not run for this change.

Local verification logs are retained under `build/final-tests.log` and
`build/example-results/`; existing checked-in example evidence was preserved.

Manual check: from `examples/record-members/`, run `./run.sh < demo.repl`
or `./verify.sh`; see that directory's README for interactive commands.
