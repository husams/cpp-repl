# Virtual method definitions

Worktree: `/Users/husam/.Codex/worktrees/replxx-example/codex/virtual-definitions`.
Branch: `codex/virtual-definitions`; baseline: `98411d1`.
The primary checkout's uncommitted work is preserved.

- [x] Compile missing non-pure virtual methods and concrete overrides.
- [x] Publish emitted vtables, RTTI and adjustment/covariant thunks.
- [x] Verify native virtual dispatch from one unchanged consumer library.
- [x] Provide configured manual examples for both requested cases.
- [x] Run regression, terminal/startup checks and independent review.

## Implementation

`%define` compiles ordinary out-of-line definitions for declared non-pure
virtual methods and concrete derived overrides. No `virtual`/`override` suffix
is added to the command signature. Pure virtual declarations remain explicitly
unsupported; this does not change abstractness or add new class declarations.

The compiler's LLVM module supplies native-visible vtables, VTTs, RTTI,
construction tables and adjusting/covariant thunks. Their actual JIT addresses
are gathered after execution and published together with the method through a
single native metadata image. Hidden/private support symbols retain visibility.
Batch preflight rejects native collisions before publishing any new export.
Publication failure closes the whole batch; a successful JIT definition remains
defined after an environmental publication error, matching the documented
existing callable contract.

## Verification (2026-10-25)

- macOS ARM64, LLVM 22.1.8: **194/194 CTest tests passed**; log:
  `build/virtual-ctest-macos.txt`.
- linux-dev x86_64, LLVM 21.1.8: **192/192 CTest tests passed**; log:
  `/tmp/cpp-repl-virtual-definitions.nTJj67/build/virtual-ctest-linux.txt`.
- Native regressions cover both requested cases, JIT-owned and native-owned
  vtables, multiple and virtual inheritance, covariant returns, RTTI,
  virtual mock/restore, rejected pure declarations and failed-definition retry.
- Terminal/startup checks load the native consumer only after virtual
  definitions; a collision regression verifies atomic native batch publication.
- Configured example `verify.sh` passed on both hosts, proving standalone loader
  failure, virtual/direct native calls and RTTI after REPL definitions, exactly
  one supplied library, and unchanged before/after SHA-256 hashes.
- Shell syntax and `git diff --check` passed. Independent review approved the
  final implementation with no remaining blockers for these Itanium platforms.

## Manual test

Directory: `examples/unresolved-virtuals/`; its `run.sh` prefers this worktree's
built executable and never silently selects an older installed REPL.

```sh
cd /Users/husam/.Codex/worktrees/replxx-example/codex/virtual-definitions/examples/unresolved-virtuals
./run.sh
```

At the prompt:

```text
virtual_demo::non_pure_probe(2)
virtual_demo::override_probe(3)
virtual_demo::rtti_probe()
```

Expected values: 42, 106, 1. See the example README for startup definitions and
how to edit their bodies for another test without rebuilding the library.
Saved native proof: `examples/unresolved-virtuals/evidence/{macos,linux-dev}.txt`.

Delivery: [PR #4](https://github.com/husams/cpp-repl/pull/4), created for the
requested merge. The manual-test worktree is retained; no installation was
performed, and the primary checkout's unrelated edits are intact.
