# Large-header editor latency

Branch: `codex/editor-large-session-latency`, based on the inline-parameter editor
worktree. The primary checkout and its unrelated changes remain untouched.

- [x] Reproduce with a generated header, rather than relying on a small session.
- [x] Sample the stalled completion and ground its import path with CTK.
- [x] Remove repeated namespace work and enable Clang's importer lookup index.
- [x] Verify real-terminal completion, typing, argument hints and execution.
- [x] Independently review the importer changes.
- [x] Complete the full regression suite: 299/299 passed in 161.27 seconds.

The fixture includes `<regex>`, `<filesystem>` and `<unordered_map>`, plus 30,000
function declarations across 3,000 reopenings of one namespace. The terminal
test also loads the project's native test library before including the header.

On macOS ARM64 with LLVM 22.1.8, the same qualified completion took 8,684 ms
before the fix and 97 ms afterwards. The native sample located the stall in
`SessionSource::completeVisibleDeclsMap`, importing declarations through Clang's
uncached declaration lookup. Retaining every namespace reopening multiplied
the work; the importer's default linear name search added another quadratic
cost when importing many distinct declarations.

The fix retains each source namespace's canonical declaration once and creates
`ASTImporterSharedState` with the destination translation unit, enabling Clang's
dedicated lookup index. All namespace redeclarations remain available, and each
analysis request retains its own disposable compiler and index.

The real-terminal fixture measured 101 ms to display the completion menu and
2.8 ms for typing echo, then completed and executed a two-argument function to
produce 42. Semantic coloring took 17 ms in the direct analysis test. The
independent reviewer reran 11 relevant tests and measured 96 ms completion.

Run the generated-header reproduction and interaction regression with:

```sh
cd /Users/husam/.codex/worktrees/embedded-clang-editor/replxx-example
ctest --test-dir build -R 'LargeHeaderSession|editor_large_session_pty' --output-on-failure -V
./build/cpp-repl
```

Evidence: `build/large-session-before.log`,
`build/large-session-before-sample.txt`, `build/large-session-after.log`,
`build/large-session-pty-after.log`, `build/large-session-ctk-final.log`,
`build/large-session-review.md`, and `build/large-session-final-full-tests.log`.

The initial full run passed the other 298 tests but exposed a shutdown issue
in the new PTY harness: blocking `waitpid` stopped draining terminal output.
The harness now drains output while polling for exit with a five-second limit;
its focused rerun passed in 1.2 seconds before the final full run.

Compiler analysis remains synchronous; this fix removes the measured quadratic
import costs. It does not promise a fixed response time for every possible
header, template instantiation or user program.
