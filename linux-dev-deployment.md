# Linux dev deployment

Host: `linux-dev`, user `husam`, x86-64, LLVM/Clang 21.1.8.
Source: performance fix `5187782` plus the LLVM 21 scope-exit compatibility guard.
The guard uses the same version split already present in the interpreter.

Release directory:
`/home/husam/.local/share/cpp-repl/releases/5187782`.
Installed command: `/home/husam/.local/bin/cpp-repl`.
The previous installed executable is retained as `previous-cpp-repl` in the
release directory. Remote source and build files are isolated from existing
checkouts; unrelated changes in the primary macOS checkout remain untouched.

Validation:

- Linux full suite: 297/297 passed in 164.53 seconds, including real-terminal
  large-header completion, semantic coloring, inline hints and argument navigation.
- macOS focused editor tests after the compatibility guard: 7/7 passed.
- Installed-release smoke: LLVM 21.1.8 banner, regex/filesystem header includes,
  a two-argument function returning 42, and retained standard-library types.
- Dynamic dependencies resolve without missing libraries.
- The Linux compatibility guard received independent review.

Run from the Mac:

```sh
ssh -t linux-dev /home/husam/.local/bin/cpp-repl
```

Or, while logged into Linux:

```sh
~/.local/bin/cpp-repl
```

The release directory retains configure/build/install logs, `tests.log`, source
revision metadata, binary checksums and smoke output. A broader standard-library
temporary-object probe reported unresolved vector/path destructors in both the
previous installed build and this build, including with the vendor driver config;
that existing LLVM 21 execution limitation is separate from these editor changes.
