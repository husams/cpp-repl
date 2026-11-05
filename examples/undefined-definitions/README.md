# Discover undefined symbols before loading C++

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
