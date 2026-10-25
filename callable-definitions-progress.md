# Callable definitions before loading a native library

Baseline: `408976e938c7fa34457b494daa95247ed7aedc16`.
Branch: `codex/define-callables`; no push requested.

- [x] Extend `%define` with signature/body syntax and AST-derived native ABI definitions.
- [x] Cover overloads and private static/non-static methods with ordinary C++ member bodies.
- [x] Add one supplied consumer library with undefined callable imports and numbered startup scripts.
- [x] Finish recovery/rejection regressions and terminal/startup coverage.
- [x] Run complete macOS LLVM 22 and linux-dev LLVM 21 suites.
- [x] Save example proof, review changes and document limitations.

## Implementation and review

``%define qualified::function(type name) cpp`body` `` selects the existing AST
declaration and compiles a real out-of-line function/member definition, using
the declaration's return type, qualifiers, exception specification and mangled
name. It publishes the resulting code address through the existing native
export mechanism before `%lib`. Private access follows ordinary member rules.

Failed parsing leaves a valid prototype in the redeclaration chain, allowing
corrected bodies to compile and later callers to execute normally. Unresolved
dependencies use the existing guarded Undo path. Existing definitions and
native implementations are rejected; documented unsupported categories report
errors instead of adapting their ABI.

The Linux aggregate fixture exposed LLVM 21 stack corruption when the existing
hook dispatch wrapper used `musttail` with `byval` stack arguments. Such
wrappers now use ordinary call lowering with all ABI attributes preserved.
Regression coverage exercises native and JIT aggregate calls, mock/restore,
non-trivial string parameters/returns, references, pointers, C linkage,
qualified methods, session compiler flags and error recovery.

Independent review approved the final implementation with no remaining code
blockers after aggregate mock/restore coverage was added.

## Saved example proof

- `examples/unresolved-callables/evidence/macos.txt`: ARM64, LLVM 22.1.8.
- `examples/unresolved-callables/evidence/linux-dev.txt`: x86_64, LLVM 21.1.8.
- Both show exactly one supplied library, all six undefined callable imports,
  standalone loader failure, successful native calls after numbered startup
  definitions, and unchanged SHA-256 hashes.
- Linux configured example: `/home/husam/cpp-repl-examples/unresolved-callables`;
  tested binary: `/tmp/cpp-repl-release-linux.tOOrpl/build/cpp-repl`.

## Final validation (2026-10-25)

- macOS ARM64 / LLVM 22.1.8: **185/185 CTest tests passed**, including PTY
  multiline recovery and numbered user/project startup loading of the native
  consumer. Complete log: `build/callable-ctest-macos.txt`.
- linux-dev x86_64 / LLVM 21.1.8: **183/183 CTest tests passed**, including the
  same PTY/startup checks. Complete log:
  `/tmp/cpp-repl-release-linux.tOOrpl/build/callable-ctest-linux.txt`.
- Both configured example `verify.sh` runs passed with all six imports
  remaining undefined in the original file and equal before/after hashes.
- Shell syntax checks and `git diff --check` passed. Existing rollback fix is
  preserved. Feature remains on the local branch; no push or install performed.
