# Supply missing callables before loading one unchanged library

This example builds **one** consumer shared library, `libcallable_consumer`,
whose final file retains six undefined callable imports. It supplies no
definitions library. The missing functions and methods exist only as
declarations in `include/api.hpp`; implementations come from REPL startup.

```sh
./configure.sh
CPP_REPL=/absolute/path/to/cpp-repl ./verify.sh
CPP_REPL=/absolute/path/to/cpp-repl ./run.sh
```

The configured scripts in `.cpp-repl/` run in numeric order:

1. `01-header.repl` loads declarations and shows unresolved session symbols.
2. `02-definitions.repl` supplies two overloaded free functions, a function
   returning a four-double aggregate, a `std::string` function, a private
   `const & noexcept` instance method using `this`, and a private static method.
3. `03-library.repl` opens the original library and includes output helpers.

The native consumer calls all six supplied definitions from its already
compiled machine code. The library contains public `Worker::run` and
`Worker::run_static` wrappers; the private implementations remain absent from
the library. `native_probe()` returns 1 only when every argument, return and
member access behaves correctly.

```text
callable_demo::native_probe()
callable_demo::Worker(40).run(2)
callable_demo::Worker::run_static(7)
callable_demo::adjust(5)
callable_demo::adjust(2.0)
```

`verify.sh` checks that exactly one shared library is supplied, inspects its
undefined file symbols, demonstrates standalone `dlopen(RTLD_NOW)` failure,
then starts the REPL with a clean user startup directory. Project startup
defines the imports before `%lib`. The script checks native/JIT results and
compares the consumer's SHA-256 before and after. No source edits, rebuild or
relink occurs between failure and success. Temporary Clang-generated export
metadata points directly at the compiled REPL implementations.

Saved proof: [macOS ARM64](evidence/macos.txt) and
[linux-dev x86_64](evidence/linux-dev.txt). The transcript reports the linked
LLVM version, undefined symbols, expected loader failure, passing native/JIT
checks, and unchanged-file hash.

Use headers and standard-library ABI matching the supplied library. Define
dependencies before their callers. Unsupported callable categories and
syntax are documented in the main README's `%define` reference.
