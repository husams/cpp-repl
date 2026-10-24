# Runtime inspection implementation

- [x] Make symbol and library lists contain names only.
- [x] Add library details: paths, versions, dependencies and ABI requirements.
- [x] Extend symbol details with definitions, imports and reference sites.
- [x] Add terminal colors, documentation and cross-platform verification.

Verified: macOS ARM64 122/122 tests and Linux x86_64 120/120 tests pass,
including terminal color checks. The build with native inspection disabled
also passes its inspection/session regression checks. Linux weak imports are
checked against ELF metadata and live import slots when omitted by the loader.

Native reference coverage will be explicit: parsed source references, import
slots and supported direct calls in the defining image. Missing debug data or
unsupported instructions must not be presented as proof of no references.

Follow-up: `%symbols` now hides platform and Homebrew system libraries by
default; `%symbols all` includes them and combines with `lib`/`name` filters in
any order. Names-only output and unrestricted `%syminfo` lookup are preserved.
Verified 27/27 focused tests on each of macOS ARM64 and Linux x86_64, including
terminal completion and system-export visibility; 24/24 compatibility checks
also pass with native inspection disabled. Manually confirmed the reported
`ztbsv` variants are hidden by default and visible with `all` on macOS.
