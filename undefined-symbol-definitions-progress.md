# Undefined symbol definition generation

Based on the 2026-11-05 draft in the personal wiki; implementation is isolated on `codex/undefined-symbol-definitions`.

- [x] Parse command and exercise source/header analysis before execution.
- [x] Generate editable definitions, with exact signatures and provider checks.
- [x] Prepare declarations, stage definitions, and commit initializers once.
- [x] Preserve generated-file edits and reject accidental overwrites.
- [x] Validate failure recovery, compilation contexts, templates and unsupported targets.
- [x] Document usage and run repository checks.

Analysis must not execute user input or mutate the live session. Staged inputs remain unexecuted until commit; unresolved dependencies leave the batch available for correction or cancellation.

## Implementation

- `acbe1c8`: isolated Clang dependency analysis and editable script generation.
- `8c86c92`: prepare/define/commit/cancel commands, staged reloads, native publication, and regressions.
- Independent correctness review: approved after resolving virtual support groups, declaration identity, staged reload cancellation, template instances, and retained/serialized header compilation contexts.
- Original checkout retained its five pre-existing modified files. No merge, push, installation, or changes to that checkout were performed.

## Usage

```text
%generate-defines source.cpp --output definitions.repl
%generate-defines api.hpp --tu source.cpp --output header-definitions.repl
```

Complete and uncomment the generated `%define` entries; retain the surrounding `%prepare-defines` and `%commit-defines`. Replay with the same configured compilation database and flags. `%cancel-defines` discards a pending unexecuted batch. `--update` preserves user edits and comments stale signatures.

The example at `examples/undefined-definitions` was run on macOS and Linux: its startup script produces `answer() == 42` and an initialization count of one.

## Validation

All 24 new regressions (23 generation/staging/grammar tests and one compilation-database test) pass on both platforms.

| Platform | Toolchain | Full CTest result |
| --- | --- | --- |
| macOS arm64 | LLVM 22.1.8 | 394/394 passed |
| linux-dev x86-64 | LLVM 21.1.8 and GCC 8.5 SDK | 389/391 passed; two independently reproduced baseline failures |

The Linux failures are `CppInterpreterTest.SymbolListingHidesSystemExportsUnlessAllIsRequested` and `pty_smoke`. A separate clean build of the original `7664d4e` baseline reproduces the same symbol-listing failure and the same three library-listing/libinfo assertions in the smoke test. No new feature test fails. These existing toolchain/library-path assertions were not changed by this feature.

Independent review approved the implementation. `git diff --check` passes. The implementation remains on the isolated branch and has not been merged, pushed, or installed.

Linux uses LLVM 21.1.8 and the GCC 8.5 SDK at `/opt/toolchain/x86_64-2.6.32-v4.0.137`, with `-DCMAKE_CXX_STANDARD_LIBRARIES=-lstdc++fs` supplied for this branch's existing filesystem linkage. Its build and fixtures remain isolated at `/tmp/cpp-repl-undefined-defines.1Mh3qk`; baseline controls are at `/tmp/cpp-repl-undefined-defines-baseline.1Mh3qk`.

Logs on the development Mac:

- `/tmp/cpp-repl-defines-macos-final.txt`
- `/tmp/cpp-repl-defines-linux-final.txt`
- `/tmp/cpp-repl-defines-linux-baseline.txt`
- `/tmp/cpp-repl-defines-linux-baseline-pty.txt`
