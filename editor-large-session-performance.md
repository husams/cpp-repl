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

## Overload-menu regression check

The final editor at `4b21a9d` was compared with the pre-menu release (`b77bba1`
on macOS; the equivalent runtime `d3901d3` on Linux). Both were Release builds
using the same host toolchain. Each process loaded the native hook test library,
then `<regex>`, `<filesystem>`, `<unordered_map>` and a deterministic header with
30,000 or 100,000 declarations across 3,000 or 10,000 namespace reopenings.
Five distinct completion queries and coloring inputs avoid hiding cold analysis
behind the result cache; typing has 50 samples per fixture.

Paired median timings in milliseconds (previous release → final editor):

| Host | Symbols | Qualified completion | Key echo | Nested-call Enter |
| --- | ---: | ---: | ---: | ---: |
| macOS, LLVM 22.1.8 | 30,000 | 135.8 → 124.7 | 0.135 → 0.130 | 59.7 → 62.3 |
| macOS, LLVM 22.1.8 | 100,000 | 433.1 → 413.7 | 0.129 → 0.130 | 149.7 → 159.7 |
| Linux, LLVM 21.1.8 | 30,000 | 217.8 → 235.0 | 0.204 → 0.200 | 105.8 → 97.3 |
| Linux, LLVM 21.1.8 | 100,000 | 802.5 → 742.6 | 0.205 → 0.214 | 323.3 → 297.1 |

All four comparisons passed the declared regression tolerances: common analysis
and Enter medians may increase by at most the larger of 20% or 20 ms; maxima by
the larger of 25% or 50 ms. Typing and cursor-edit medians may increase by 10 ms,
with maxima below 250 ms. Menu navigation, acceptance and argument typing must
stay below 100 ms; menu opening below two seconds. Observed typing maxima were
about 0.6 ms or less, navigation below 19.2 ms, and selected-hint acceptance below 24.2 ms.
Semantic coloring, single/multiple/nested calls, zero-argument calls, cursor edits,
evaluation results and undecorated history were also checked.

The comparison first exposed redundant Enter analyses and a selected-hint delay
inside the typing debounce. Enter now reuses metadata and limits fresh analysis
to explicit gaps, completion-created calls and the existing trailing-call path.
Complete manually typed expressions retain ordinary Clang diagnostics. Selecting
an overload seeds its parameter metadata immediately; ordinary typing keeps its
deferred semantic analysis.

Run the reproducible benchmark against each binary:

```sh
python3 -I tests/editor_large_context_benchmark.py BASELINE NATIVE_LIBRARY --iterations 5 --skip-menu --json-out before.json
python3 -I tests/editor_large_context_benchmark.py CURRENT NATIVE_LIBRARY --iterations 5 --expect-menu --json-out after.json
# Add --stress to both commands for the 100,000-declaration fixture.
```

Fixture SHA-256 values are
`433e1da108694d19085a7e3603472f4fd38136c6bfbffe99e88be43492769efd`
and `75aede0418554848a40f5cb1ed84c3e1447725b0a974d7f96fce1ea090bfec6e`.
Raw samples and comparison results are retained as
`build/large-context-{macos,linux}-{before,after,comparison}-{30000,100000}.json`
and in the isolated Linux release directory.
