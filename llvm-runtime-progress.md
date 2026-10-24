# Clang/LLVM runtime backend

- [x] Replace library, export and import inspection with LLVM Object and OS loaders.
- [x] Replace instrumentation-library memory and branch-island helpers with native OS APIs.
- [x] Replace the Linux instruction decoder with LLVM.
- [x] Remove the external instrumentation dependency and update build documentation.
- [x] Verify all tests on macOS ARM64 and Linux x86_64.

Verification: macOS ARM64 127/127 and Linux x86_64 125/125 tests pass,
including precompiled private callers, pre/mock/post hooks, restore, GOT calls,
symbol/library inspection and terminal tests. Linux code-page permissions are
checked before, during and after hooking; library files remain unchanged.
The disabled-native build passes all 114 non-terminal tests. A fresh macOS
configure/build contains no instrumentation devkit fetch or link target, and
both production binaries contain no old instrumentation or decoder symbols.

Linux preserves RX permissions through /proc/self/mem rather than removing and
restoring executable permission, which execmod policy rejects for dirty
file-backed pages. macOS uses Mach copy-on-write page protection. Branch islands
are written as RW, then made RX, using native OS allocation APIs.
