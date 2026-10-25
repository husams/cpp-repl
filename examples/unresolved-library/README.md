# One precompiled library with undefined static members

This example builds **one shared library** containing a class whose static
members are declared but never defined. No source file or companion library
supplies their storage. The numbered startup scripts use `%define` to create
that storage in the REPL **before** `%lib` opens the existing library.

The final library retains these undefined imports:

```text
unresolved_demo::State::title    U
unresolved_demo::State::total_   U
unresolved_demo::State::samples  U
unresolved_demo::State::weights  U
```

Linux also prints the standard `[abi:cxx11]` suffix on the string symbol.
The library alone fails to load. After the REPL defines the members, native
functions in that unchanged library can read and write the same storage.

## Already configured locations

Mac:

```sh
cd /Users/husam/workspace/replxx-example/examples/unresolved-library
./run.sh
```

linux-dev:

```sh
ssh linux-dev
cd /home/husam/cpp-repl-examples/unresolved-library
./run.sh
```

The installed `cpp-repl` must include automatic native publication for
`%define`. Both configured locations retain the built library and scripts.

## Declaration and startup definitions

`include/state.hpp` declares the members; `src/consumer.cpp` contains only
native functions that use them. In particular, `State::total_` is private.
There is no `definitions.cpp` or user-supplied shim.

The startup directory contains:

```text
.cpp-repl/
  0-search.repl       Add the built library's directory
  10-definitions.repl Load declarations, find missing members, define them
  20-consumer.repl    Load the single consumer library
```

`10-definitions.repl` is the complete definition setup:

~~~cpp
%add include/state.hpp
%symbols undefined name '^unresolved_demo::State::'
%define unresolved_demo::State::total_ cpp`6 * 7LL`
%define unresolved_demo::State::title cpp`std::string("provided") + " at startup"`
%define unresolved_demo::State::samples cpp`std::vector<int>{3, 5, 8}`
%define unresolved_demo::State::weights cpp`{2, 4, 6}`
~~~

`20-consumer.repl` then opens the already built library:

```text
%lib unresolved_consumer
#include <iostream>
```

Change the initializers in `10-definitions.repl` and start a new session to
choose different values. Rebuilding the library is unnecessary. The REPL
obtains the actual types and mangled names from the header, including the
private scalar and array declarator. No names or values are special-cased.

## Manual test

At the prompt:

```cpp
unresolved_demo::verify();
%syminfo unresolved_demo::State::total_
%syminfo unresolved_demo::State::title
```

The function executes native code from the library and prints:

```text
Native private scalar: 42
Native string: provided at startup
Native vector: 3, 5, 8
Native array: 2, 4, 6
NATIVE_LIBRARY_PASS
```

Native calls and REPL expressions share the same object. For example:

```cpp
unresolved_demo::State::title = "changed in REPL";
std::cout << unresolved_demo::read_title() << '\n';
```

This prints `changed in REPL` through a function compiled into the library.
Symbol information shows an undefined, resolved import in the consumer plus
the address and import slot. Its symbol table on disk still contains `U`.

## How the address reaches the native loader

`%define` initializes the actual object using Clang's persistent interpreter,
then looks up its JIT address. The REPL automatically uses Clang to generate
a tiny temporary native image whose absolute export points at **that same
address**. This image has no second allocation, C++ definition, or initializer.
It makes the address visible to `dlopen` on macOS and Linux.

This is internal loader metadata: you do not write, build, configure, or
preload a shim. The configured Clang executable must be available. The export
images and JIT storage are retained for the session. Thread-local storage
cannot be published through a single absolute address and is rejected before
its initializer runs.

## Reproduce the retained proof

```sh
./configure.sh  # initial build; already done on both machines
./verify.sh     # tests existing binaries without rebuilding
```

The verification:

1. Checks the build directory contains exactly one shared library, reads
   that **final shared library**, and checks all four members have `U`
   entries in its symbol table.
2. Attempts to load it in a separate process without definitions and checks
   the missing-symbol failure refers to a static member of `State`.
3. Starts the installed REPL; the startup scripts define the members before
   loading the library and verify their values through native calls.
4. Compares native and REPL string addresses, then verifies a native function
   sees a value written through the REPL.
5. Compares SHA-256 hashes before and after, proving the library is unchanged.

Verification isolates user startup scripts using a temporary home directory;
ordinary `run.sh` launches honor your user `~/.cpp-repl/` first.

- [Mac transcript](evidence/macos.txt)
- [linux-dev transcript](evidence/linux-dev.txt)

Mac builds the deliberately unresolved library with
`-undefined dynamic_lookup`. Linux permits these imports in a shared library.
Both use Clang and ABI-compatible standard-library headers. The standalone
`loader_probe` is an executable used only to demonstrate the initial failure;
it supplies no symbols or storage.

The complete suites passed 180/180 tests on Mac and 178/178 on linux-dev,
including a native-library regression that failed before address publication
was added and a check that unsupported TLS initializers do not run.
