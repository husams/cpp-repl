# Rollback crash reproduction

An input with an unresolved symbol must report an error and leave the session
usable. Clang 21/22 can instead crash while undoing an input containing an
explicit `using namespace` or the implicit using-directive created by an
anonymous namespace.

`anonymous.cpp` is the reduced version of a reported `%add` crash: an anonymous
namespace followed by a global initialized from an undefined external variable.

Build cpp-repl from the repository root, then run:

```sh
cmake --preset default
cmake --build --preset default
examples/undo-crash/verify.sh ./build/cpp-repl
```

On Linux, configure with the `linux` preset and your LLVM/Clang CMake locations,
then run `examples/undo-crash/verify.sh ./build-linux/cpp-repl`.

The script checks three independent sessions: `%add anonymous.cpp`, inline
anonymous-namespace input, and inline `using namespace` input. Every case must
report `error: undefined symbol: undo_missing`, allow both rejected variables
to be redefined, print `(int) 9`, and exit successfully. It returns nonzero if
the process crashes or rollback leaves either declaration behind.

Before the fix, the `%add` case exited 139 on macOS with LLVM 22.1.8 and 132
on linux-dev with LLVM 21.1.8. The fix detaches the rejected input's top-level
using-directives before calling Clang's `Undo()`, retaining full rollback.
The regression suite also checks lookup isolation, nested directives, source
retry after defining the import, and preservation of installed function hooks.

This workaround covers explicit rollback after successful parsing. Clang's
internal cleanup of syntax errors is unchanged.
