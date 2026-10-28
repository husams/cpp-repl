# replxx-example: cpp-repl

`cpp-repl` is a C++ REPL. It uses Clang's incremental interpreter
(`clang::Interpreter`, the engine behind `clang-repl`) to compile each input on
top of the earlier ones and run it in-process with the ORC JIT. The line editor
is [replxx](https://github.com/AmokHuginnsson/replxx), with tab completion,
hints, C++ syntax highlighting, multiline editing, persistent history, custom
key bindings (Enter, Ctrl+L) and clean Ctrl+C / Ctrl+D handling.

```text
cpp-repl> int twice(int x) {
          return x * 2; }
cpp-repl> twice(21)
(int) 42
cpp-repl> #include <iostream>
cpp-repl> std::cout << "hi" << std::endl;
hi
cpp-repl> %lib z
loaded: libz.dylib
cpp-repl> extern "C" const char* zlibVersion();
cpp-repl> zlibVersion()
(const char *) "1.2.12"
cpp-repl> struct Point { int x, y; int sum() const { return x + y; } };
cpp-repl> Point pt{1, 2};
cpp-repl> pt.<Tab>
sum  x    y
cpp-repl> %class Point
struct Point {  // size 8, align 4
  int x;  // +0, 4 bytes
  int y;  // +4, 4 bytes
  int sum() const;
};
```

Value expressions print by default, including expressions ending in `;`.
`%set print_values = false` disables automatic result output. Clang's diagnostics go to
stderr, and the session stays usable after an error. Two guards cover clang 22
problems:
- An input that references a symbol the JIT can't resolve is rejected with
  `error: undefined symbol: ...` before it runs. Otherwise ORC would keep
  retrying the failed module on every later input.
- A `return` outside any braces is rejected, because it crashes the parser.

replxx is fetched with CMake `FetchContent`, pinned to commit
`2b248467112cbbc16f63fde38230fd7c7ffc55f1` (tag `release-0.0.4`).

## Build and test

Requires CMake >= 3.25 (presets), Ninja, Python 3 for the pty test, and LLVM/Clang
21 or 22 with development headers and CMake packages (`brew install llvm` on
macOS). The `default` preset builds with
Homebrew's clang against `/opt/homebrew/opt/llvm`, so the interpreter matches
LLVM's ABI. For another location, override `CMAKE_C_COMPILER`,
`CMAKE_CXX_COMPILER`, `LLVM_DIR` and `Clang_DIR`.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

On Linux x86_64, use a matching Clang/LLVM installation and the Linux preset:

```sh
cmake --preset linux -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build --preset linux
ctest --preset linux
./build-linux/cpp-repl
```

If Clang's CMake package is outside the normal search paths, also pass
`-DClang_DIR=/path/to/lib/cmake/clang`. LLVM 21 uses the REPL's formatter for
scalar values, pointers and C strings because its built-in value printer is
not implemented; LLVM 22 uses Clang's value printer.

LLVM 21/22 can crash while mangling a lambda directly in a top-level expression
([upstream fix](https://github.com/llvm/llvm-project/commit/5835ef200a1fdd0aa92578ee824772eea4143279)).
Assign the lambda to a variable before calling it, for example
`auto f = [] { return 4; };` followed by `f()`.

Pass `-DREPLXX_EXAMPLE_BUILD_TESTS=OFF` to skip the tests (and googletest).

## Run

```sh
./build/cpp-repl                 # any arguments are compiler flags:
./build/cpp-repl -std=c++20 -Iinclude
```

REPL commands start with `%` or `!`, and `$option` queries a session setting.
Other input is C++: it is highlighted, then compiled and run by the interpreter.

Percent commands other than `%set` can substitute shell output into their arguments using double
backticks. Each block launches `/bin/sh -c` in the REPL's current directory,
with its environment, and replaces the block with captured stdout. Trailing
newlines are removed; other newlines remain and separate arguments where the
command grammar permits. Stderr goes directly to the terminal. A failed shell
process reports its exit status and prevents the REPL command from running.
Child stdin is `/dev/null`, so scripts cannot consume subsequent REPL input.

```text
%lib ``./find_lib.sh``
%lib ``
  ./find_lib.sh
``
%add ``printf '%s\n' first.cpp second.cpp``
%add-path "``printf '/path/with spaces'``"
%add-compile-opt ``printf -- '-DVALUE=42'``
%clear
```

Enter continues an open double-backtick block, including blank lines; Ctrl+C
cancels it. The shell runs once on submission, including in startup scripts;
editing, highlighting and completion do not execute it. Quote the whole block
when its output is one argument containing spaces, using the destination
command's quoting rules. Multiple blocks run left to right. Captured output is
never expanded again. Ordinary C++ input and existing `cpp` backtick bodies
keep their literal contents; substitutions belong outside those bodies.

`%type <symbol>` prints a declared type without evaluating the symbol. Functions
use `(parameter types) -> return type`; a bare overloaded name prints one line
per overload. Qualified names, members, aliases and typed overload selectors use
the same AST lookup as `%syminfo`; quote selectors containing spaces.

```text
int x = 0;
%type x
(int)
int f(int x) { return x + 1; }
%type f
(int) -> int
! pwd
/my/path
! printf '%s\n' 'hello world' | tr a-z A-Z
HELLO WORLD
```

`! <shell-command>` (or `!command`) runs one physical line through `/bin/sh -c`
in the REPL's current directory and environment. Stdout and stderr stream to the
REPL unchanged, including output produced before a failure; a nonzero exit or
signal prints an error and returns to the prompt. Child stdin is `/dev/null`,
so commands cannot consume later REPL input. Each command has a separate shell;
`! cd /tmp` does not change the REPL directory. Shell quoting, pipes and redirects
are passed through verbatim. Editing and completion never execute the command.
A leading `!` selects the shell; parenthesize a C++ negation, such as `(!x)`.

- `%clear` - clear the screen and return to the prompt, preserving the session.
- `%lib [path ...]` - `dlopen` zero or more shared libraries, separated by
  blanks. A path containing `/` is opened directly; otherwise each
  `%add-path` dir is tried, then the system lookup, as `<name>`,
  `lib<name>.dylib` and `lib<name>.so` (e.g. `%lib z c++`). No argument lists
  the loaded libraries.
  The native loader is tried first. If dependencies are missing, LLVM reads
  their names and the REPL tries the requesting library's directory, then
  `%add-path` directories, recursively. This supports sibling and nested
  dependencies without changing the library files or rebuilding them.
- `%add-path <path[:path...]>` - add one or more library search directories,
  separated by `:` (each must exist), e.g. `%add-path /opt/lib:~/lib`.
- `%compile-opts [file]` - show current global options or the effective options
  for a source file or header. Output includes the selected compilation
  database command, global flags, file flags, working directory, and combined
  flags, with aligned fields and terminal colors. With no filename, it also
  lists explicitly configured file overrides.
- `%add-compile-opt [file] <flags...>` - append flags globally or for one file.
  Omit the filename when changing globals. File flags take precedence over
  globals, and globals take precedence over compilation database options.
- `%use-compile-opts <file>` - use the file's compile settings as the session
  reference. This copies its database options and file overrides into the
  session defaults, retaining manual global flags. Relative database paths
  retain their original build-directory meaning; the REPL stays in its current
  directory. The file is not compiled or added to the session by this command.
  The defaults apply to later C++ input, hooks, and added files. Switching to
  another reference replaces the previous reference's settings. This is a
  snapshot: after changing a file's options, run the command again to refresh
  the session defaults. `%compile-opts` shows the selected reference, and a
  global `%set-compile-opts` replaces or clears it.
- `%set-compile-opts [file] [flags...]` - replace the flags for that scope.
  Globally, this replaces all global flags, including startup command-line
  flags. For a file, this replaces its database options and previously added
  file flags; global flags still apply. Supplying no flags clears that scope;
  an empty file replacement uses only global and built-in platform settings.
  Changes affect future compilation, including interactive C++ and hook
  bodies for globals. Existing declarations, objects, and compiled hooks
  remain in the session; added files are not automatically recompiled.
  Updates are validated before being stored, and rejected updates preserve
  the previous options.
  A filename is the optional first word before the flags; there is no `--`
  separator. Quote filenames, include directories, or flag values that contain
  spaces. Single/double quotes and backslash escapes group compiler arguments;
  expansion and command execution require explicit double-backtick blocks.
  Relative manual include paths resolve from the directory where the command is entered,
  even when the database command uses a different build directory. File
  settings can be recorded before the source file exists. For filenames
  beginning with `-`, use `./-filename.cpp`.

  ```text
  %compile-opts
  %add-compile-opt -std=c++20 -I "include directory"
  %add-compile-opt src/point.cpp -DTRACE_CALLS=1
  %set-compile-opts src/point.cpp -std=c++20 -DTRACE_CALLS=2
  %compile-opts src/point.cpp
  %use-compile-opts src/point.cpp
  %set-compile-opts -std=c++17 -DGLOBAL_VALUE=42
  %set-compile-opts src/point.cpp
  %set-compile-opts
  ```

- At startup, the REPL loads scripts from **directories** in this order:
  first `~/.cpp-repl/` for user defaults, then the nearest project `.cpp-repl/`
  found in the startup directory or its ancestors. Only the nearest project
  directory is loaded; discovery stops at the user's home directory or the
  filesystem root. The user directory is loaded once, including when the
  project path is a symlink to it.
  Name each script `<number>-<name>.repl`, for example:

  ```text
  ~/.cpp-repl/
    0-options.repl
    2-types.repl
    10-helpers.repl
  ```

  Scripts execute in numeric order within each directory before the first
  prompt (`2` before `10`); all user scripts finish before any project script,
  regardless of their numbers. Project commands can append to user flags or
  replace them with `%set-compile-opts`.
  Leading zeros are allowed, and equal numbers sort by filename. Other files
  and subdirectories are ignored. A missing directory is silently ignored.
  An ordinary file at `~/.cpp-repl` reports an error; put startup inputs in
  numbered scripts inside the directory instead.

  Each script accepts C++ and `%commands`, just like the prompt, including
  multiline C++ and hook bodies with blank lines. Use C++ `//` or `/* */`
  comments; `#include` and other preprocessor directives remain C++ input.
  Use `%add` to load source/header files. Commands and C++ run in one shared
  session. User scripts resolve relative paths from the directory where the
  REPL starts. Project scripts resolve them from the project root containing
  `.cpp-repl/`, even when launched from a subdirectory. The prompt returns to
  the original working directory after project scripts finish. Use `~/` or
  absolute paths in user scripts to make them independent of the startup
  directory.

  For example, `~/.cpp-repl/0-options.repl`:

  ```text
  %use-compile-opts ~/workspace/project/src/main.cpp
  %add-compile-opt -std=c++20 -DREPL_SESSION=1
  %compile-opts
  ```

  `~/.cpp-repl/2-types.repl`:

  ```cpp
  #include <iostream>
  ```

  `~/.cpp-repl/10-helpers.repl`:

  ```text
  %add ~/workspace/project/include/common.hpp
  %add "~/workspace/project/src/session helpers.cpp"
  ```

  Project-specific settings can instead live in
  `~/workspace/project/.cpp-repl/0-options.repl`:

  ```text
  %use-compile-opts src/main.cpp
  %add-compile-opt -DPROJECT_SESSION=1
  %add src/session_helpers.cpp
  ```

  The options script runs before either later script. Start the REPL from the
  project directory so its `compile_commands.json` is discovered. Errors report
  the script path and input's starting line; later inputs and scripts still
  run. An unfinished C++ or hook block is reported at that script's end, and
  loading resumes with the next script. `%quit` exits immediately, skipping
  later inputs and scripts.

- `%add [path ...]` - parse and run C++ source files in the session, as if
  each were `#include`d. Diagnostics then point into the file, and its own
  `#include "..."` lines resolve next to it. Each file can be added once. Its
  functions, variables and classes can be used, completed and shown with
  `%class` like typed input. With no argument it lists the added files.
  Earlier declarations remain available after a failed input; see the
  [include recovery example](examples/include-recovery/README.md) for the
  checked reproducer and compiler-state limits.
  At startup, the REPL searches the current directory and its ancestors for
  `compile_commands.json`, also checking `build/` and `build-linux/` at each
  level. The first database found is validated and loaded once using Clang
  Tooling; malformed JSON reports its path and prevents startup. The startup
  message names the selected database.
  Each added file uses its exact compile command. If it has no entry, a C++
  entry from the same directory supplies the options, preferring a matching
  filename stem and then alphabetical order. This also covers headers and
  `.cc`, `.cxx`, and other C++ extensions. Files without a matching entry or
  same-directory fallback use the REPL's normal options; missing files still
  report an error.
  Include directories, definitions/undefinitions, language settings such as
  `-std=c++20`, forced includes, and macro includes apply while adding that
  file. Relative paths resolve against the compile command's directory;
  command-line REPL flags override database flags. Both JSON `command` and
  `arguments` forms and compiler response files are supported. Output and
  dependency-file flags are removed, and the REPL's directory, live objects,
  and hooks are preserved, including after a failed addition.
  Files still share one persistent AST: already parsed headers and declarations
  remain in the session. Compilation databases supply preprocessing and language
  settings; warning policy, optimization, debug emission, linker options, and target code generation
  remain controlled by the running JIT. Commands for incompatible targets,
  per-file PCH, or VFS overlays report an error.
  See the [compilation database example](examples/compilation-database/README.md)
  for a checked demo of per-file entries, same-directory fallback, and choosing
  an entry as the session defaults.
- `%reload [path ...]` - reread previously added C++ files and their user
  headers, using each file's current compile options. With no arguments it
  reloads all added files. Edits are also detected automatically before the
  next C++ input; there is no background execution while you are editing.
  Syntax or unresolved-symbol errors preserve the last working definitions
  and macros. Initializers run again on a successful reload; external effects
  from executed C++ cannot be undone.
- Ordinary C++ definitions can be entered again at the same prompt: classes,
  functions, variables, aliases, enums and templates use the newest definition
  in subsequent inputs. For example:

  ```cpp
  class A { public: int value() { return 1; } };
  A old;
  class A { public: int value() { return 2; } };
  A current;
  current.value()  // 2
  old.value()      // 1
  ```

  Existing objects, pointers and compiled callers keep their original types
  and definitions, including after file reload. Create new objects/callers to
  use the new version; values of an old class type do not become instances of
  its new definition. Retaining old versions uses memory until the session
  ends. Standard C++ rules still apply within class bodies and to explicit
  template specializations; C linkage and precompiled library ABI changes
  cannot be versioned this way.
- `%class [name]` - show a class defined in the session: its bases and
  members as declared, its size and alignment, and each field's offset and
  size, all read from the interpreter's AST. A name can be qualified
  (`geo::Vec2`) or a typedef. With no argument it lists the classes from user
  code: your inputs, `%add`-ed files and their own headers. Classes from
  system headers (the standard library, the SDK) are left out.
- `%methods <name> [access public|private|protected|all] [include-parents true|false]`
  and `%fields <name> [access public|private|protected|all] [include-parents true|false]`
  list methods or data members of a class, struct or union already parsed in
  the session. Names can be qualified or typedefs, as with `%class`.
  Defaults are `access all` and `include-parents false`; options may appear in
  either order. Output groups fully qualified member names and types under
  their declaring record, with the record itself first and bases traversed
  depth first in declaration order. Each ancestor appears once, including in
  diamond hierarchies. Access filters use the member's declared access, even
  through private inheritance. Fields include static data members; methods
  include declared constructors, destructors, operators, overloads and method
  templates. Compiler-generated implicit members, nested types and inherited
  `using` declarations are omitted. An empty group says `no matching methods`
  or `no matching fields`. Queries read the retained AST without executing code.
  See [the manual example](examples/record-members/README.md).
- `%mock <signature> [in <caller signature>] cpp\`<body>\`` - replace a function or method body.
- `%pre <signature> [in <caller signature>] cpp\`<body>\`` - run immediately before each call.
- `%post <signature> [in <caller signature>] cpp\`<body>\`` - run after a normal return, including a
  return from a mock. For non-void functions, `result` holds the return value
  and can be changed. Post hooks do not run when a call throws.
- `%restore [signature [in <caller signature>]]` - remove the selected function or method's mock and
  both hooks together, restoring calls to its original body. Without a signature,
  remove all mocks and hooks in the session.
- `%hooks` - list all active mocks and hooks with their kind (`%mock`, `%pre`
  or `%post`), registered signature and full original C++ body in `cpp\`...\``
  syntax. Multiline bodies preserve their whitespace and comments. Cleared and
  restored entries are omitted; an empty session prints `no active mocks or hooks`.
- `%libs [all] [regex]` - list loaded library names, including dependencies;
  no paths, columns or counts. System libraries are hidden by default, using
  the same locations as `%symbols`; `%libs all` includes them. An optional regex
  filters names, including after `all` (for example `%libs all '^libc[.]'`).
  Quote `'all'` to use that word as a regex instead of an option. Bare `%lib` lists only
  libraries explicitly loaded with `%lib`, also as names only.
- `%paths` - show added library directories in search order, executable and loaded
  library RPATH/RUNPATH entries, loader environment variables and dependency
  fallback rules. Output is aligned and colored in terminals. OS loader caches
  and system defaults remain loader-managed; environment values shown are the
  current process values, which the native loader may have cached at startup.
- `%libinfo <library>` - show a loaded library's full path, recorded version,
  base address, architecture, dependencies and ABI metadata. Accepts a name,
  full path or regex. Linux shows the running glibc version and recorded
  GLIBC, GLIBCXX, CXXABI and other version requirements/definitions. macOS
  shows Mach-O current/compatibility versions, install name and UUID, including
  metadata available from loaded dyld-cache images. Missing metadata is labeled
  unavailable or not recorded; a SONAME is not treated as a package version.
- `%demangle <symbol>` - print the readable C++ name of a mangled symbol using
  LLVM, without requiring its library to be loaded. Accepts Linux `_Z...` and
  macOS `__Z...` names; plain or unrecognized names are printed unchanged.
  For example, `%demangle _Z3addii` prints `add(int, int)`, and
  `%demangle __ZN5Point3incEi` prints `Point::inc(int)`. Quote names containing
  spaces. Tab completion and `%help` include the command; startup scripts can
  use it too.
- `%syminfo <symbol>` (also `%info`) - inspect a qualified C++ name (such as `Calculator::add`),
  a mangled name, or a JIT symbol. Shows aligned fields for the declaration,
  type, linkage, definition, source location and available address. Functions
  also show the preserved original body and active global or caller-specific
  hooks. All overloads are shown separately. Variables, classes, fields, aliases
  and namespaces are supported; types also show size/alignment where available.
  Native exports can be inspected without headers. Definition/resolution state,
  undefined imports, parsed source references, import slots and supported direct
  calls in the defining image are shown. Quote names with spaces, for example
  `%syminfo 'Point::inc(int)'` or `%syminfo 'add(int, int)'`.
  Typed signatures also select overloads from session declarations, including
  functions loaded with `%add`. Supply parameter types without parameter names
  or argument expressions; method `const`/`volatile` and reference qualifiers
  distinguish overloads. Declared aliases and canonical parameter type spellings
  may be mixed per parameter. Reference parameters accept prefix/postfix cv
  spellings for aliases too, preserving pointer-level qualifiers and the
  distinction between `&` and `&&`. Queries read AST metadata without parsing
  or executing C++ input.
- ``%define <symbol> cpp`initializer` `` - provide a missing definition for a
  declared global variable or static data member. Clang supplies the actual
  type and declarator; you supply any valid C++ initializer, including function
  calls, constructed objects, arrays, pointers, references, and multiline
  expressions. Names and values are not built into the implementation. Private
  static members are defined through ordinary C++ member-definition rules.
  Existing C++ definitions and loaded-library storage are preserved: the
  command rejects a duplicate without executing its initializer. Missing
  declarations and invalid initializers report errors, and a rejected
  initializer can be corrected and retried.
  For example, after loading a header declaring `app::State::value`:

  ```text
  %symbols undefined
  %define app::State::value cpp`make_value()`
  %syminfo app::State::value
  ```

  Put header declarations in an earlier numbered `.repl` script, definitions
  in the next, and `%add` inputs which use them afterward. `%lib` supplies
  existing exported data from native libraries; `%define` creates storage
  in the REPL's JIT and publishes its address to the native loader. Load a
  library's header, `%define` its missing data, then `%lib` the original library.
  No user-written shim or rebuild of the supplied library is required. Clang
  generates temporary native export metadata pointing at the same JIT storage;
  it does not create a second object or run its initializer again. The library
  file retains its undefined imports. The configured Clang executable must be
  installed. Thread-local storage cannot be published this way and is rejected
  before initialization. If native publication fails after initialization, the
  error explicitly reports that the variable remains defined in the session.
  The [unresolved-library example](examples/unresolved-library/README.md)
  demonstrates a final shared library retaining undefined static members,
  resolved by `%define` before loading that single supplied library.
  See [the configured static-member example](examples/static-members/README.md)
  for numbered startup scripts, a native library, and recorded Mac/Linux runs.
- ``%define <signature> cpp`body` `` - define a missing free function or static/
  non-static method and publish its actual JIT address before `%lib` loads a
  library which imports it. Supply the qualified name, parameter types and
  names, and member `const`/`volatile`/reference qualifiers; the return type is
  optional. Clang derives the return type, parameter declarators, exception
  specification and mangled name from the existing declaration. For example:

  ```text
  %add include/api.hpp
  %define app::adjust(int x) cpp`return x + 7;`
  %define app::adjust(double x) cpp`return x * 2.5;`
  %define app::Worker::hidden(int x) const & noexcept cpp`
  return this->value_ + x;
  `
  %define app::Worker::scale(int x) cpp`return x * 3;`
  %lib supplied_consumer
  ```

  Bodies contain ordinary C++ statements, with `this` and direct access to
  private members inside real member definitions. Clang compiles the actual
  function/method, preserving native C++ argument and return conventions,
  including references, function pointers, aggregate and `std::string` values.
  Overloads are selected by canonical parameter types and member qualifiers.
  Unnamed parameters get `arg0`, `arg1`, etc.; use qualified, accessible type
  names in signatures. The declaration determines `noexcept`, even if omitted
  from the command. Empty bodies are allowed for `void` functions.

  Concrete template instances are supported, including methods on class
  template instances, declared method specializations, and function template
  instances. For example:

  ```text
  %define std::string Person<int>::getName() const cpp`return "Sample";`
  %define describe<int>(int value) cpp`return std::to_string(value);`
  ```

  Clang selects the overload using parameter types,
  member qualifiers, and the return type when provided; the REPL supplies the
  required `template<>` prefixes. Define an instance before its first use.
  Runnable examples are in `examples/template-callables`.

  As with variables, the user supplies no shim and never rebuilds or relinks
  the supplied library: generated temporary native metadata exports the JIT
  address, while imports in the original file remain undefined. Define
  dependencies first; unresolved bodies fail without poisoning later inputs.
  Existing definitions and resolved native implementations are rejected.
  Constructors, destructors, operators, dependent primary-template patterns,
  constexpr functions, variadic functions, pure virtual declarations, internal
  linkage, and non-default calling conventions are unsupported and report
  errors. Signature parsing supports qualified template-ids, ordinary
  parameter declarators and optional `const`, `volatile`, `&`, `&&`, `noexcept`;
  computed exception specifications and other suffixes are rejected.
  If publication fails after compilation, the error states that the callable
  remains defined in the session. Native consumers must stay within the
  interpreter session's lifetime and use compatible headers, target and C++
  standard-library ABI.
  See [the configured callable example](examples/unresolved-callables/README.md)
  for one supplied library, numbered startup scripts, loader-failure checks,
  native-call verification and unchanged-file hashes on Mac and Linux.
  Non-pure virtual methods and concrete overrides are supported using the
  existing declaration's qualified name, without adding `virtual` or `override`
  to the command signature. Clang emits the method and its required vtables,
  RTTI, and adjustment/covariant thunks; `%define` publishes native-visible
  definitions of those support symbols alongside the method's code address.
  Each method and its support symbols are published together in one native
  metadata image; failed publication does not leave a partial set of new native
  exports. As with other callable definitions, a compilation that already
  succeeded remains defined in the JIT after a publication error; start a fresh
  session to retry after fixing that environment error.
  This supports native-owned and JIT-owned vtables, multiple/virtual inheritance
  and covariant returns. Hidden support symbols retain their original visibility.
  Define dependent methods first, including other undefined virtual slots,
  before the key method that emits a class's vtable. A pure virtual declaration
  stays pure; supply an already-declared concrete override in the derived class.
  [The virtual-method example](examples/unresolved-virtuals/README.md) provides
  manual tests for a non-pure virtual method and a concrete override, both called
  by one unchanged native library after startup definitions.
- `%symbols [all] [undefined] [lib <regex>] [name <regex>]` - list loaded native exports
  and emitted session functions/globals as demangled, fully qualified C++ names
  only, one per line, with no columns or counts.
  Add `undefined` to list unresolved user declarations and unresolved imports
  from loaded libraries instead. This finds missing static data members even
  before an expression references them. Names already resolved from a library
  or defined in the session are omitted; regex filters still apply, including
  `lib '<session>'` for parsed declarations. `all` includes system libraries.
  System libraries are hidden by default; `%symbols all` includes them.
  System locations are `/System/`, `/Library/Apple/`, `/usr/lib/`, `/usr/lib64/`,
  `/lib/`, `/lib64/` and Homebrew's `/opt/homebrew/`, `/usr/local/Cellar/` and
  `/usr/local/opt/` (plus Linux's virtual runtime image).
  The `all`, `lib` and `name` options can appear in any order; filters still apply
  with `all`. A `lib` filter alone does not reveal system libraries.
  Use `lib '<session>'` to select session definitions.
- `%help` - list the commands.
- `%quit` - exit (Ctrl+D also exits; plain `quit`/`exit` are C++ input).

Grammar (see `src/commands.hpp`):

```
lib       := "%lib" { path }
libs      := "%libs" [ "all" ] [ regex ]
paths     := "%paths"
libinfo   := "%libinfo" library
add       := "%add" { path }
reload    := "%reload" { path }
compile-opts := "%compile-opts" [ file ]
add-compile-opt := "%add-compile-opt" [ file ] flag { flag-or-value }
set-compile-opts := "%set-compile-opts" [ file ] { flag-or-value }
use-compile-opts := "%use-compile-opts" file
class     := "%class" [ name ]                  name := ident { "::" ident }
members   := ("%methods" | "%fields") name [ "access" access ]
             [ "include-parents" ("true" | "false") ]
access    := "public" | "private" | "protected" | "all"
hook      := ("%mock" | "%pre" | "%post") signature [ "in" signature ] "cpp`" body "`"
restore   := "%restore" [ signature [ "in" signature ] ]
hooks     := "%hooks"
info      := "%info" symbol
syminfo   := "%syminfo" symbol
type      := "%type" symbol
shell     := "!" shell-command                 single physical line
demangle  := "%demangle" symbol
define    := "%define" (symbol | signature) "cpp`" (initializer | body) "`"
symbols   := "%symbols" [ "all" ] [ "undefined" ] [ "lib" regex ] [ "name" regex ]
add-path  := "%add-path" path { ":" path }     blanks around ':' are allowed
help      := "%help"
quit      := "%quit"
path      := '"' chars '"' | word              quote paths that contain blanks
```

For example, after loading the points example library:

```text
%symbols lib libpoints
%symbols lib libpoints name '^Point::'
%symbols name '^add\(int, int\)$'
%symbols lib '<session>' name '^demo::'
%symbols all
%symbols all lib 'libc[.]|libSystem' name '^malloc$'
%libs libpoints
%libs all
%libs all 'libc[.]|libSystem'
%paths
%libinfo libpoints
%syminfo 'Point::inc(int)'
```

Filters are case-sensitive ECMAScript regular expressions, searched within the
full library path and the readable symbol name. Both filters must match when
supplied together, in either order. Single or double quotes group expressions
containing spaces; backslashes are preserved. An empty regex matches everything.
Invalid regexes report an error and leave the session usable. Names are sorted
and deduplicated; a filter with no matches produces an empty list.

Lists and detail blocks use terminal colors automatically. Labels are aligned,
names/labels are cyan, addresses are yellow, and resolved states are green.
Piped output contains no color escapes; `NO_COLOR` or `TERM=dumb` disables color.
Symbol kinds belong in `%syminfo`, keeping list output easy to copy.

Reference sites are explicitly partial: parsed source expressions, native import
slots, and supported native calls in the exporting image. Indirect calls, inlined
calls, hidden callers and calls in other native images may be absent. Direct-call
scanning is skipped for images with more than 2048 exports and is reported as
such. A zero count means no known sites, not proof of no references. Source
locations inside precompiled libraries require matching debug/source data and
are not invented from addresses. Inspection never installs a hook.

Native exports can be listed before including any headers. Binary export metadata
identifies functions and variables; including a library's declarations also lets
the REPL identify methods, constructors and destructors. Vtables and type-info
exports get their own kinds. Hidden/stripped symbols, undeclared C signatures,
and class/namespace types without runtime symbols cannot be reconstructed from
exports. Session declarations without emitted definitions are omitted.
Native enumeration is enabled by default on macOS ARM64 and Linux x86_64;
`-DREPLXX_RUNTIME_SYMBOLS=OFF` retains session-only listing. Native inspection uses
LLVM Object with the OS loader APIs (dyld on macOS, `dl_iterate_phdr` on Linux).

For example, `%info demo::add` produces a plain-text block with aligned labels:

```text
Symbol: demo::add
  Kind        : function
  Declaration : int demo::add(int a, int b)
  Type        : int (int, int)
  Mangled     : _ZN4demo3addEii
  Linkage     : external
  Definition  : yes
  Location    : input_line_1:1:22
  Address     : 0x...
  Original IR : __cpp_repl_original_1
  Original    : 0x...
  Hooks       : none
```

Addresses vary per session. `Address` is the public entry point; `Original` is
the preserved body before mocking. For library targets, `Address` and `Original`
refer to the exported native function; `Routing IR` and `Routing` identify the
separate JIT entry used by REPL calls. An inline method that has not been emitted,
or an unresolved declaration, reports an unavailable address. A field requires
an object; types and namespaces have no address. `%info` does not call the
inspected function or evaluate an expression. Tab completes session symbol names.

Input that breaks the grammar (unknown command, missing or empty path,
arguments to `%help`/`%quit`, unterminated quote) prints the error and the
usage, and runs nothing.

A `%` line submits on Enter, except an open `cpp\`` block, which continues
until the closing backtick. Backticks inside C++ strings and comments are
ordinary content. These blocks also work in piped input.

### Function mocks and hooks

Define or `%add` the target first, then use its qualified name and parameter
types to select an overload. The return type is optional; when supplied it
must match. Include `const`, `volatile` and reference qualifiers for methods.
Name parameters in the signature to use them in the body (unnamed parameters
are exposed as `arg0`, `arg1`, etc.). Names can differ between hooks.

```text
cpp-repl> int twice(int x) { return x * 2; }
cpp-repl> %pre twice(int x) cpp`x += 1;`
cpp-repl> %mock twice(int x) cpp`
          return x + 10;
          `
cpp-repl> %post twice(int x) cpp`result *= 2;`
cpp-repl> twice(3)
(int) 28
cpp-repl> %mock twice(int x) cpp``
cpp-repl> twice(3)
(int) 16
```

Each target has one pre hook, one mock and one post hook; setting one again
replaces it. An empty body (`cpp\`\``) clears only that hook. Execution order
is pre → mock (or original body) → post. Pre hooks can change arguments before
the body runs. Hooks also apply to earlier compiled callers, recursive calls,
function pointers and virtual calls within the session. Failed updates keep
the previous hooks active.
Use `%restore twice(int x)` to clear all three at once; it also succeeds if
the selected target already has no hooks. Restoring one overload leaves other
overloads unchanged. Bare `%restore` clears every mock and hook in the session;
functions, objects and other session declarations remain available.
The interpreter disables function inlining to preserve these call boundaries,
including when compiler optimization is requested.

To change only calls made directly inside one function or method, add
`in <caller signature>`. Target and caller signatures both select exact
overloads, with the return type optional:

```text
cpp-repl> int add(int x) { return x + 1; }
cpp-repl> struct Calc { int inc(int n) { return add(n); } int double_it(int n) { return add(n)*2; } };
cpp-repl> Calc calc;
cpp-repl> %mock add(int x) in Calc::inc(int n) cpp`return 100;`
cpp-repl> calc.inc(3)
(int) 100
cpp-repl> calc.double_it(3)
(int) 8
cpp-repl> add(3)
(int) 4
cpp-repl> %restore add(int x) in Calc::inc(int n)
```

The filter affects every direct call to that target in the selected caller,
including callers compiled earlier. Calls through another helper, a function
pointer, or virtual dispatch are not selected. A caller without a supported
direct call is rejected. Use a non-virtual explicit call if a virtual method
needs caller filtering.
Private and protected methods can be selected as targets or callers. Native
addresses are resolved through the JIT using mangled symbol names. Deferred
session definitions are emitted through Clang's AST code-generation API, with
no generated `&Class::method` expression or change to access checking.
Ordinary REPL input and hook bodies still follow normal C++ access rules.

Each caller/target pair has its own pre, mock and post set. While that set is
active, it takes precedence over the target's global set and uses the original
target body when its own mock is absent. Other callers continue through the
global set, or the original body if no global hooks exist. Clearing the scoped
set restores this fallback. `%hooks` includes the `in` filter and C++ body.
`%restore <target> in <caller>` clears one pair; `%restore <target>` clears its
global set and every caller-specific set; bare `%restore` clears the session.

On macOS ARM64 and Linux x86_64, the same caller filter also supports two functions in a
precompiled library, including private methods. Load the existing library and
its declarations, then select the native caller normally:

```text
%lib ./libpoints.dylib
#include "point.hpp"
#include <iostream>
%pre add(int a, int b) in Point::inc(int) cpp`std::cout << "pre" << std::endl;`
%mock add(int a, int b) in Point::inc(int) cpp`return a+b+100;`
%post add(int, int) in Point::inc(int) cpp`result += 10;`
%restore add(int, int) in Point::inc(int)
```

On Linux, load `./libpoints.so` instead; the hook commands are identical.

This redirects only native call/tail-call instructions inside `Point::inc` that
target `add`. Native calls from `double_it` keep
their original behavior. `%restore` reinstates the original instructions;
session destruction restores any remaining patches. No library source or
on-disk binary is changed or rebuilt. Both addresses must be resolvable, the
process must permit code patching. macOS uses Mach-O function-start metadata
and supports ARM64 `B`/`BL` plus standard `ADRP`/`LDR`/`BR` linker stubs.
Linux requires glibc, ELF function sizes and section metadata. It decodes x86_64
instruction boundaries and handles relative `CALL`/`JMP`, normal/CET PLT entries,
and RIP-relative GOT calls such as `-fno-plt` output. Each matched instruction
must have at least five bytes; short or conditional branches to the target
are rejected before any changes. Inlined calls, dynamic indirect/virtual calls
and unrecognized stubs are unsupported. ELF libraries are bound eagerly so
their PLT/GOT destinations can be matched before the first call.
Apply changes while the affected native code is idle.
Callers whose entry address is shared by multiple exported symbols are rejected.
Native caller filters use their own hook set; global library hooks continue
to affect JIT-generated calls only.

Native hooks use Clang/LLVM and native OS APIs, with no additional instrumentation
dependency. LLVM decodes Linux x86_64 instructions; LLVM Object reads binary
metadata. Branch islands use `mmap`/`mprotect`. Linux updates private code through
`/proc/self/mem`, preserving executable-page permissions even with execmod
restrictions; macOS uses Mach copy-on-write protection APIs and restores the
original page permissions.
Configure with
`-DREPLXX_NATIVE_CALL_PATCHES=OFF` to disable this backend. Other platforms keep
support for caller filtering within JIT-generated code. 32-bit Linux x86 is not
supported by the native backend.

For instance methods, use `self->member` or `this->member` to access public
members; bare member names are not brought into scope. `self` has the method's
const/volatile qualification. Hooks affect all instances of the selected
method. `self`, `result` and names starting with `__cpp_repl_` are reserved.

```text
cpp-repl> struct Counter { int n; int get() const { return n; } };
cpp-repl> %mock int Counter::get() const cpp`return this->n + 100;`
cpp-repl> Counter{2}.get()
(int) 102
```

Targets can have a body compiled in this session, including `%add`-ed files, or
an exported symbol in a loaded library. For a library, load it with `%lib` and
include its header (or declare the function) before selecting its signature.
Global hooks redirect calls and function pointers generated by the REPL,
including earlier JIT callers. Calls inside a library keep their native
destinations unless selected by a supported native caller filter as described
above. Function pointers returned by native code are not redirected. Symbols
absent from the library's exports cannot be looked up.
Unresolved declarations, templates, constexpr functions, variadic functions,
constructors/destructors and operator-name signatures are unsupported.
Parameters passed by value must be trivially
copyable; use references for types such as `std::string`. Hook code has the
session's global scope and accesses members using ordinary C++ access rules.

- **Highlighting** (`src/cpp_lexer.*`, `src/highlight_cpp.*`): keywords, built-in
  and std types, strings/chars with escapes (including raw strings), numbers
  (hex, binary, float, `'` separators, suffixes), comments, preprocessor lines,
  operators, and brackets colored by nesting depth (unmatched ones in red).
  `%` lines are colored by the command grammar: command names bright green,
  paths bright blue, `:` separators yellow, and the first grammar error (an
  unknown command, a dangling `:`, extra arguments...) bright red. Works across
  embedded newlines. Embedded Clang analysis overlays semantic identifier
  colors: types bright cyan, namespaces bright blue, functions bright green,
  methods green, fields cyan, parameters yellow, variables light gray, and enum
  constants bright magenta. The overlay resolves the current edited buffer
  against session declarations without executing it. Strings, comments, command
  operands and shell blocks keep their existing colors. Unresolved or incomplete
  constructs retain lexical coloring; buffers larger than 64 KiB use lexical
  coloring alone. Repaints reuse results until the buffer, session or flags change.
  Analysis currently runs synchronously; large sessions or header-heavy edited
  input can delay a repaint.
- **Multiline**: Enter is bound to a handler that inserts a newline in the same
  buffer while the input is incomplete (open `() [] {}`, unterminated
  string/char/raw string, open `/*`, trailing `\` in a `#` line, or a line
  ending in an operator or comma) and submits otherwise. The block stays
  editable and becomes one history entry. Blank lines continue an incomplete
  block; Ctrl+C cancels it. Ctrl+J always inserts a newline.
- **Completion** (Tab): embedded Clang semantic completion in a disposable
  compiler importing the session's current declarations; no LSP is required.
  Tab extends the common name prefix; another Tab lists colored suggestions
  with parameter names, return/data types and kind labels. Overloads appear as
  separate rows. Tab on an overloaded name or an unselected empty call opens
  a signature menu; Up/Down selects a row and Tab accepts it. The chosen
  signature supplies the gray parameter hints and argument stops. Escape closes
  the menu, and ordinary editing dismisses it. Selection guides the editor;
  C++ still resolves the call from the argument types.
  Completing a function or method inserts `()` and leaves the
  cursor at the first argument. Light-gray hints inside the parentheses show
  parameter names, blank argument slots and types, with the return type after `)`.
  Type an argument and press Tab to move to the next slot; Shift+Tab returns
  to the previous slot, and Tab after the last argument moves past `)`.
  These hints are display-only: they never enter executed C++ or history.
  Enter keeps the annotations on a completed call's terminal line by default,
  including when the cursor has moved past `)` or `;`. Parameter names and types
  also remain on calls inside expressions, such as `add(1, 2) + 20`; a return
  annotation appears only when no expression code follows that call.
  Required empty arguments keep the call editable on Enter; default arguments
  may be omitted. Ordinary comma-separated typing also works. Existing call
  parentheses are reused, and completing in the middle of a name replaces its
  remaining characters. Suggestions
  follow Clang's context/type priorities and omit inaccessible or unavailable
  members. Completion handles locals in unfinished multiline input, inheritance,
  `const` receivers, namespaces and member access after calls. While entering a
  function call, the same inline parameter hints appear after a short pause.
  After `#`, suggestions follow Clang's preprocessor context: directive keywords,
  macro operands or conditional-expression macros, rather than unrelated C++
  symbols. Unique prefixes such as `#def` complete immediately; `#i` has several
  valid matches, including `if` and `include`.
  Header-name completion uses Clang's configured include search paths, including
  quoted, angle-bracket and nested paths.
  Tab also completes `%` commands, filesystem paths after `%lib `,
  `%add-path `, `%add ` and `%reload `, and class names after `%class `,
  `%methods ` and `%fields `. The member commands also complete their option
  keywords, access values and parent traversal booleans.
  The pinned replxx dependency carries local completion and display extensions;
  `cmake/replxx-display.patch` preserves its existing insertion and pagination
  behavior. `scripts/editor-grounding.ctk` traces the AST analysis and rendering
  paths using this build's compilation database.
- **Paired delimiters**: typing `(`, `[`, `{`, `"` or `'` closes the pair and keeps
  the cursor inside. Typing an existing closer moves past it; Backspace between
  an empty pair removes both characters. Include directives also pair `<` with
  `>`; ordinary C++ angle brackets retain normal typing. Quotes, escapes, comments,
  raw strings and numeric separators keep their C++ meaning. Pairing applies to C++ input
  and embedded C++ command bodies; shell and path operands retain normal typing.
- Semantic colors and parameter hints appear after a short pause in typing;
  lexical colors remain immediate and Tab completion runs on demand. Function
  addresses and `using` declarations complete names without inserting a call.
- Before the interactive prompt, the editor caches the initial sequence of
  successful preprocessor-only inputs (such as startup `#include` directives)
  as a precompiled header. Later session declarations are imported on demand,
  and parameter hints request signatures without enumerating all symbols.
  The cache lives in `$XDG_CACHE_HOME/cpp-repl/editor-pch`, or
  `~/.cache/cpp-repl/editor-pch` when `XDG_CACHE_HOME` is unset. Compiler settings,
  working directory and header contents identify the cache. If a captured header
  changes during a session or Clang rejects the cache, the editor falls back to
  importing the existing session declarations. Editor analysis remains synchronous.
- History persists in `./.replxx_history`; Ctrl+L clears the screen; Ctrl+D
  (or `%quit`) exits.

### Session options

Use `%set <option> = true|false` to change an option and `$option` to show its
current value. Tab completes option names and boolean values. Unknown options
and invalid assignments report an error without changing session settings.

| Option | Default | Behavior |
| --- | --- | --- |
| `print_values` | `true` | Print non-void expression values, including calls and assignments ending in `;`; `false` mutes automatic result output. |
| `keep_hints` | `true` | Retain gray call annotations after Enter; `false` removes them from the submitted line while keeping editing hints. |

```text
cpp-repl> int add(int a, int b) { return a + b; }
cpp-repl> add(10, 20);
(int) 30
cpp-repl> %set print_values = false
print_values = false
cpp-repl> $print_values
print_values = false
cpp-repl> %set keep_hints = false
keep_hints = false
```

Expressions execute once in either output mode. Declarations, void expressions
and control flow have no synthetic result; explicit program output is unaffected.
Semicolon expressions requiring temporary-object cleanup execute with ordinary
C++ lifetimes and may omit automatic value output: Clang's value-capture rewrite
can lose those cleanups. This includes class results with nontrivial destructors.
When one input contains several expressions, Clang's last captured value is shown.
Options apply immediately within the session. Put `%set` commands in startup
`.repl` files to reuse your preferences in later sessions; `$option` is a standalone
query, rather than substitution inside C++ source.

Non-interactive smoke test (ordinary C++ is read line by line; hook blocks
can span multiple lines):

```sh
printf 'int n = 6;\nn * 7\n%%quit\n' | ./build/cpp-repl
```

## Agent skill

The [cpp-repl skill](skills/cpp-repl/SKILL.md) guides Codex and Claude through
reproducing C++ behavior, inspecting symbols and libraries, testing hooks, and
debugging the REPL itself. Install the complete folder so its references remain
available:

```sh
mkdir -p ~/.codex/skills ~/.claude/skills
cp -R skills/cpp-repl ~/.codex/skills/
cp -R skills/cpp-repl ~/.claude/skills/
```

Invoke it with `$cpp-repl` in Codex or `/cpp-repl` in Claude Code; matching tasks
can also select it automatically.

## Layout

- `src/main.cpp` - creates the interpreter, input loop, history, Ctrl+C/D
- `src/cpp_interpreter.cpp` - `clang::Interpreter` wrapper: platform flags
  (resource dir, libc++, SDK), execution, value printing, symbol check,
  `%lib` hand-over to the JIT, code completion
- `src/cpp_inspect.cpp` - AST queries: user classes, `%class` synopsis,
  `%methods` / `%fields` member lists and
  layout, member names for completion
- `src/project_compilation_database.cpp` - startup database discovery and
  Clang Tooling compile-command selection and adjustment
- `src/source_compile_options.cpp` - scoped per-file frontend options,
  include search paths and macro restoration
- `src/startup_files.cpp` - user scripts, then nearest-project numbered `.repl` scripts
- `src/cpp_symbols.cpp` - `%info` symbol metadata, overload lookup and aligned output
- `src/function_hooks.cpp` - overload lookup, per-function JIT routing,
  mock and hook compilation
- `src/repl.cpp` - replxx callbacks, key bindings, command execution
- `src/cpp_lexer.cpp` - C++ lexer, incompleteness check, member-access
  detection for completion
- `src/highlight_cpp.cpp` - token to color mapping
- `src/commands.cpp` - `%` command table, parsing, dispatch, path completion
- `src/library_manager.cpp` - `%add-path` search paths and `%lib` dlopen
  handles; a load hook registers each library with the interpreter
- `src/highlight_repl.cpp` - picks command vs. C++ highlighting
- `tests/cpp_lexer_test.cpp`, `tests/commands_test.cpp`,
  `tests/cpp_interpreter_test.cpp` - googletest unit tests;
  `tests/pty_smoke.py` - pty test
