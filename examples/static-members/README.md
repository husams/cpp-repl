# Missing static-member definitions from startup scripts

For static members that are undefined **inside a final precompiled library**,
see [the unresolved-library example](../unresolved-library/README.md). This
example compiles the consuming code in the REPL.

This example is configured and verified with the installed `cpp-repl` on
macOS ARM64 and linux-dev x86_64. The declarations and consuming source contain
**no definitions** of `State`'s four static members. Numbered project startup
scripts provide their storage and initializers through `%define`.

The separate native library already defines `Native::baseline`. Loading the
library resolves that member normally; the startup scripts leave its value at
91. All names and values here are example data, not special cases in the REPL.

## Run the configured example

On this Mac:

```sh
cd /Users/husam/workspace/replxx-example/examples/static-members
./run.sh
```

On linux-dev:

```sh
ssh linux-dev
cd /home/husam/cpp-repl-examples/static-members
./run.sh
```

At the prompt, on either platform:

```cpp
storage_demo::check();
%symbols undefined name '^storage_demo::'
%syminfo storage_demo::State::counter_
%syminfo storage_demo::Native::baseline
```

`check()` returns true and prints:

```text
Private scalar: 42
String: startup storage
Vector: 3, 5, 8
Array: 2, 4, 6
Native member: 91
Compile flags: 73, 29
STATIC_MEMBERS_PASS
```

The undefined-symbol query prints no names. Symbol information identifies
`counter_` as a session definition with a resolved address, and `baseline` as
a resolved native export from `libstatic_members_native.dylib` on Mac or
`libstatic_members_native.so` on Linux. Addresses vary between runs.

## What loads, in order

```text
.cpp-repl/
  0-options.repl        Select session.cpp's compile options; append extra flags
  10-declarations.repl Load the native library and the unmodified header
  20-find-missing.repl List the four members still lacking definitions
  30-definitions.repl  Define their storage using arbitrary C++ initializers
  40-use.repl          Load the source that uses those members
include/state.hpp      Declarations and the class's existing public API
src/native.cpp         Defines only Native::baseline
src/session.cpp        Uses State's members without defining them
build/                 Built library and compile_commands.json, ready to use
```

The definition script contains:

~~~cpp
%define storage_demo::State::counter_ cpp`storage_demo::initial_counter()`
%define storage_demo::State::title cpp`std::string("startup") + " storage"`
%define storage_demo::State::samples cpp`
{
  3,

  5,
  8
}
`
%define storage_demo::State::weights cpp`{2, 4, 6}`
~~~

The REPL derives each type from the header, including the private scalar,
standard-library objects, and array declarator. The blank line inside the
vector initializer is intentional. No source edits, repeated types, or access
control flags are needed. The scalar is read through the class's existing
`current()` method.

`0-options.repl` selects the compile database entry for `src/session.cpp`, then
overrides its flag value of 19 with 29. The runtime check verifies both the
database flag (73) and override (29). CMake also compiles an object target to
generate the reference entry; that object is not linked into the native
library or loaded by the REPL.

Ordinary launches load user `~/.cpp-repl/` scripts first, then this project's
scripts. The project also loads when launching `cpp-repl` from its `include/`
or `src/` subdirectory. Relative paths in the project scripts resolve against
this example's root.

## Rebuild and reproduce the proof

From the example directory, on either platform:

```sh
./configure.sh
./verify.sh
```

`configure.sh` builds with Clang and generates the local compile database.
`verify.sh` uses the installed `cpp-repl`, checks the four initial unresolved
names, all runtime values, the compile flags, and the empty final unresolved
list. It exits nonzero on failure and writes `evidence/macos.txt` or
`evidence/linux-dev.txt`. It isolates user startup scripts with a temporary
home directory and leaves the real user configuration untouched. Set
`CPP_REPL=/absolute/path/to/cpp-repl` to test a different binary.

Recorded successful runs:

- [macOS transcript](evidence/macos.txt)
- [linux-dev transcript](evidence/linux-dev.txt)
- [macOS launch from include/](evidence/macos-subdirectory.txt)
- [linux-dev launch from include/](evidence/linux-dev-subdirectory.txt)

The first Linux run exposed a compiler-option scope bug: keyword refresh was
undoing Clang's compatibility treatment of libstdc++ member names. That bug was
fixed in the REPL and covered by a regression test before recording these
successful runs; this example does not work around it by changing the headers.
The complete test suites now pass 180/180 on Mac and 178/178 on linux-dev,
including that regression and terminal tests.

These definitions also publish their JIT storage addresses to the native
loader automatically. The [unresolved-library example](../unresolved-library/README.md)
uses this to load a single precompiled library with missing static definitions,
without a user-written shim or changes to that library.
