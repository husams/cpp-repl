# Generic missing static-member storage

- [x] Discover unresolved declarations with `%symbols undefined`, retaining regex filters.
- [x] Resolve existing loaded-library storage without replacing it.
- [x] Add ``%define <symbol> cpp`initializer` `` using the AST's actual C++ type.
- [x] Verify arbitrary names and scalar, object, array, pointer, reference, and private members.
- [x] Cover startup ordering, recovery, native definitions, and terminal completion.
- [x] Document behavior and validate on macOS and linux-dev.

Initializers are supplied C++ expressions, not inferred defaults. Production
lookup uses AST declarations and mangling, with no special variable names or
assumed values. Existing definitions must not be overwritten. A symbol absent
from every library needs a definition; lookup cannot invent its value.

Full suites passed 172/172 on macOS ARM64 (LLVM 22.1.8) and 170/170 on
linux-dev x86_64 (LLVM 21.1.8), including terminal completion, private members,
runtime expressions, objects, arrays, pointers, references, native-library
definitions, startup definitions before dependent files, and recovery after
both rejected initializers and unresolved initializer functions. Clang does
not register failed parsing inputs as undoable PTUs; failed variable definitions
are demoted to invalid declarations so retrying preserves earlier session state.

## Reviewable example

- [x] Add `examples/static-members/` with numbered project startup scripts,
  private scalar/string/vector/array definitions, and existing native storage.
- [x] Configure and build the example on Mac and linux-dev.
- [x] Reproduce a libstdc++ keyword regression exposed by the Linux example;
  preserve Clang's intentionally reverted keywords across compiler scopes.
- [x] Record final installed-binary verification and subdirectory startup runs.

After the keyword fix, the complete suites passed 173/173 on Mac and 171/171
on linux-dev, including the new regression and terminal tests. Both installed
binaries passed `examples/static-members/verify.sh`; direct launches from the
example's `include/` directory also passed. Transcripts are retained under
`examples/static-members/evidence/`. The Linux copy remains configured at
`/home/husam/cpp-repl-examples/static-members`.

## Native-library scenario with a provider (superseded)

The requested case keeps static members undefined in the final built consumer
library, not only in JIT inputs. `examples/unresolved-library/` covers that
case with native calls. At that intermediate stage `%define` was JIT-only,
so the example preloaded a separate native definitions provider through `%lib`,
then opened the unchanged consumer. Verification checked its four undefined symbol-table entries, a
baseline missing-symbol load failure, native runtime reads, shared storage
between REPL and library, and an unchanged SHA-256 hash.
Both installed-binary runs pass on Mac and linux-dev. The configured Linux
copy is `/home/husam/cpp-repl-examples/unresolved-library`; transcripts remain
in `examples/unresolved-library/evidence/` for review.

## Single supplied library, no user shim

- [x] Reproduce failure when a JIT-only definition precedes native `dlopen`.
- [x] Prove absolute exports can publish a JIT address on Mac and Linux.
- [x] Automatically generate native export metadata from `%define` using Clang;
  the image aliases existing JIT storage and has no user definitions/initializers.
- [x] Verify native scalar/object calls against a single undefined fixture.
- [x] Replace the companion-provider example with `%define` before `%lib`.
- [x] Validate and install on Mac and linux-dev; retain exact proof transcripts.

Complete suites passed 175/175 (Mac) and 173/173 (linux-dev), including terminal
tests. The native library regression failed before this implementation and
now passes on both platforms. Constant objects are explicitly emitted before
address lookup; TLS publication is rejected before running an initializer.
The preceding companion-provider approach is superseded: the example builds
exactly one shared library and has no definitions source or provider target.

Before pushing, fast-forwarded to upstream rollback fix `dfdac2e` and preserved
it in the combined sources. All 180 Mac tests pass. All 178 Linux tests pass
after syncing the upstream example fixture and rerunning the two affected
tests. The one-library proof also passes with both freshly rebuilt binaries.
