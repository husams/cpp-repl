# Sibling dependency loading and search-path inspection

- [x] Reproduce sibling dependency failures with real shared libraries on both platforms.
- [x] Add regression coverage for direct, nested and missing dependencies, and rejected-load cleanup.
- [x] Resolve missing dependencies from the requesting library's directory and added paths.
- [x] Add `%paths` to display configured and loader search-path information.
- [x] Verify on macOS ARM64 and Linux x86_64 using LLVM and native OS APIs.

Before the fix, loading a library by its full path or through `%add-path` failed
when its sibling dependency was absent from native loader paths: an unresolved
`@rpath` dependency on macOS and a missing ELF DT_NEEDED library on Linux.
The regression tests were run before the fix and all three failed on macOS.

Native loading is still attempted first. On failure, LLVM reads dependency names
and missing libraries are loaded recursively from their requester's directory
and explicitly added search paths. Staged handles are released on failure or
registration rejection; binary files are never rewritten.

Verification: full suites passed 131/131 on macOS and 129/129 on Linux, followed
by 6/6 final focused checks on each platform including the additional rejection
cleanup regression and terminal completion/formatting. The disabled-native
build passed all 119 non-terminal tests. Manual before/after reproduction on
both systems now loads the same unmodified libraries and returns 42; `%paths`
prints the expected added directories, executable RPATH/RUNPATH and environment.
