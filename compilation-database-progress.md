# Compilation database integration

- [x] Capture failing tests for per-file flags and startup database loading.
- [x] Load compile_commands.json once at startup using Clang Tooling.
- [x] Select exact entries or same-directory C++ fallback, including headers.
- [x] Apply compilation settings while preserving existing REPL state.
- [x] Validate on macOS and Linux, retaining sibling-library dependency resolution.

Clang Tooling handles JSON command parsing, response-file expansion, command
transfer, and output/dependency argument adjustment. Native Clang option parsing
removes source and compile-only operands before constructing a disposable
frontend for each selected command. Its language options, include paths, and
preprocessor definitions apply only while adding that source to the existing
interpreter. No session code is replayed. CLI flags take precedence.

Regression tests cover startup snapshots, malformed JSON, per-file C++20/C++17
and macro settings, quoted relative include paths, same-directory header/.cc
fallback, missing entries/files, function-like macros, -U, -include, -imacros,
response files, output stripping, failed-add recovery, live values and mocks,
and preservation of the REPL's working directory. Invalid forced headers and
macro headers report errors without losing the session.

The persistent AST retains earlier declarations and header guards. Preprocessing
and language settings are per-file; warning policy and JIT code generation stay
session-wide. Incompatible targets, per-file PCH, and VFS overlays are rejected.

Verification: full suites passed 141/141 on macOS ARM64 (LLVM 22.1.8) and 139/139
on linux-dev x86_64 (LLVM 21.1.8), including terminal tests and all four sibling
dependency regressions. The disabled-native Linux configuration passed all 13
focused compilation-database and dependency tests. Manual REPL sessions on both
platforms loaded an unchanged shared library with a sibling dependency, called
its function successfully (42), added a C++20 source using database flags (42),
and displayed library paths with %paths.
