# Includes after rejected inputs

Clang 21/22 can erase an earlier declaration from translation-unit name lookup
when a later input redeclares it and fails. Reopening `namespace std` is enough:

```text
cpp-repl> #include <string>
cpp-repl> namespace std { int missing(); } int result = std::missing();
error: undefined symbol: std::missing()
cpp-repl> #include <string>
cpp-repl> std::string text = "ss";
```

Before the fix the last input reports `use of undeclared identifier 'std'`.
Including `<string>` again does nothing because its guard is still defined.
A syntax error such as `namespace std { int broken = ; }` has the same effect.
After the fix, the error is reported and `std::string` remains usable.

Build and run the checked example from the repository root:

```sh
cmake --preset default
cmake --build --preset default
sh examples/include-recovery/verify.sh ./build/cpp-repl
```

On Linux, use the `linux` presets and pass `./build-linux/cpp-repl`.
The script uses an empty home and working directory to exclude startup scripts.
It checks a control session, undefined-symbol rollback, syntax-error cleanup,
and `%add` of `failed.cpp`, which includes `<string>` for the first time inside
the rejected input. Every case must subsequently print `(int) 2` for the size
of a new string. The script exits nonzero on a failed check.

The application restores missing earlier redeclarations after both cleanup
paths, including each lost overload. A namespace first supplied by a system
header remains reachable because Clang retains its contents and include guard.
New namespaces from rejected user code remain hidden.

This is a name-lookup workaround, not a complete transaction for all compiler
state. Clang still retains preprocessor changes and declarations nested inside
reopened namespaces after rejected inputs. The workaround does not restore
discarded executable code or globals from a rejected header. Custom guarded
headers first included in a rejected input may still need a fresh session.
On LLVM 21, a syntax error in the same input that first includes `<string>`
can leave template constructor code unavailable even though `std::string`
lookup recovers. Include headers in a successful input before trying code
that may fail to avoid that separate code-generation limitation.
