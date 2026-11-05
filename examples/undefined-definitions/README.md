# Discover undefined symbols before loading C++

## Test manually

From this directory, run:

```sh
./manual.sh
```

The launcher uses this feature worktree's built binary and copies the two C++
files to a fresh temporary directory. Nothing is automatically prepared or
defined. It prints the directory and the commands to try:

```text
%generate-defines consumer.cpp --output definitions.repl
%prepare-defines consumer.cpp
%define definition_demo::adjust(int value) cpp`return value + 7;`
%define definition_demo::bias cpp`35`
%commit-defines
answer()
definition_demo::initialization_count
```

The last two expressions should print **42** and **1**. Inspect the generated
`definitions.repl` in the printed directory to see the discovered signatures.
It is retained after exit so you can edit it. `%cancel-defines` lets you discard
a prepared batch and try again.

To demonstrate the original failure, start a fresh manual session and enter
`%add consumer.cpp` first. It reports undefined `adjust` and `bias`; the
scan-and-define workflow above can then resolve them.

The same commands are available as a runnable transcript:

```sh
./manual.sh < manual-session.repl
```

## Start with completed definitions

From this directory, start `cpp-repl`. The numbered startup script prepares
`consumer.cpp`, supplies its missing definitions, and commits the batch.

```text
answer()                               // 42
definition_demo::initialization_count   // 1
```

The header itself contains an initializer which requires `adjust` and `bias`.
Loading the header normally before supplying them would encounter undefined
symbols. The staged path keeps its declarations available without running it.

To try generation first, start `cpp-repl` from the repository root so this
example's startup script does not run:

```text
%generate-defines examples/undefined-definitions/api.hpp
%generate-defines examples/undefined-definitions/consumer.cpp --output definitions.repl
```

Edit the generated file, supplying the body and initializer and uncommenting
the `%define` entries. It already contains `%prepare-defines` and
`%commit-defines` in the required order. Replay the commands at the prompt,
or save the completed script under a project's `.cpp-repl/10-definitions.repl`
and start a new session in that project.

Use `--update` with the same input and output to refresh a generated file.
Edits inside matching definition regions and handwritten text elsewhere are
preserved. Changed signatures and removed candidates are commented as stale.
