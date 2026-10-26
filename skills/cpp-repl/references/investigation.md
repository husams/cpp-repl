# Investigation recipes

Use only the recipe relevant to the symptom. Commands below are REPL input
unless a block is marked `sh`; put backticks in quoted shell heredocs.

## Project compilation and includes

Start in the project directory when its compilation database/startup scripts
are relevant. Query before overriding:

```text
%compile-opts src/worker.cpp
%use-compile-opts src/worker.cpp
%add include/worker.hpp
```

`%use-compile-opts` copies settings into session defaults; it does not add the
file. Rerun it after changing that file's settings. Manual file flags override
manual global flags, which override compilation-database flags. For an override:

```text
%add-compile-opt -std=c++20 -I "/path/with spaces/include"
%add-compile-opt src/worker.cpp -DTRACE=1
%compile-opts src/worker.cpp
```

`%set-compile-opts [file] [flags...]` replaces that scope; no flags clears it.
There is no `--` separator before the optional filename. Quote paths and flag
values containing spaces. Use `./-name.cpp` for filenames starting with `-`.
Relative manual include paths use the command's working directory; database
paths retain their recorded build-directory meaning. Settings do not rebuild
existing declarations or objects. All files still share an AST, so this does
not reproduce every independent translation unit or linker setting of a build.

## Native-library and undefined-symbol failures

First separate parse/type errors, missing definitions, loader dependency errors
and incompatible ABI/architecture. Gather evidence without calling the suspect
function:

```text
%paths
%libs
%libinfo libexample
%symbols lib 'libexample' name '^app::'
%symbols undefined
%syminfo 'app::Worker::run(int)'
```

`%libinfo` requires a loaded library; if loading failed, preserve the loader
diagnostic and inspect the binary/dependencies externally. A full `%lib` path
removes name ambiguity; `%add-path` assists dependency lookup. System libraries
are hidden by default even with a `lib` filter; add `all` when appropriate.
Filters are case-sensitive ECMAScript regex searches over full paths/readable
names. No matches is an empty list. Bare qualified names show the declaration or
overload set; typed signatures select an overload in current builds, including
session functions. Demangle exact failing import names. If a typed query misses,
inspect the bare qualified name, then use the printed alias spelling, canonical
parameter types or actual mangled name shown in metadata. Check for an older
executable if simple typed queries also fail. A query mismatch alone is not
evidence of an undefined function. Hook/restore signatures remain a
separate grammar; do not assume their parameter-name syntax works in `%syminfo`.

If the intended experiment needs a missing definition, load its declarations,
then supply the definition **before** loading its native consumer:

```text
%add include/api.hpp
%symbols undefined lib '<session>'
%define app::State::value cpp`42`
%define app::adjust(int x) cpp`return x + 7;`
%lib /absolute/path/to/libconsumer.so
%syminfo 'app::adjust(int)'
```

Use the real platform extension (`.dylib` on macOS). `%define` defines a missing
declaration; `%mock` routes calls to an existing implementation. Do not substitute
one for the other. C++ argument types, target architecture, headers and standard
library ABI must match the native consumer. Define dependencies first; session
code/storage cannot outlive the REPL process. Native publication uses the
configured Clang executable and generated export metadata; it does not rebuild
the supplied library or create a second variable object.

Subfeatures depend on the binary's revision: newer releases support concrete
template instances, non-pure virtual methods and declared concrete overrides.
Check the matching README and examples (`template-callables`,
`unresolved-virtuals`) before using them. Do not try to turn a pure virtual
declaration into a concrete override or define a dependent primary-template
pattern. For vtable-emitting key methods, define other missing virtual slots
first. Constructors/destructors, operators and other unsupported signatures
must be treated as capability limits, not solved with guessed mangled names.

If publication fails after successful initialization/compilation, the diagnostic
can say the definition remains in the session. Do not blindly retry and assume
rollback; fix the environment and use a fresh process.

## Select an overload without running it

Both `%syminfo` and `%info` accept a bare name, a typed signature or a mangled
name. Typed queries use parameter types without names or return types, plus
method `const`/`volatile` and `&`/`&&` qualifiers where needed. They accept the
declaration's printed alias spelling, canonical parameter types and ordinary
parameter whitespace. Canonical const-reference types accept prefix/postfix
const forms; those transformations do not apply to every alias spelling.
For example, given aliases `ns::Count = int` and `ns::Ratio = double` in a
declaration `ns::take(ns::Count, const ns::Ratio&)`, query that printed signature
or `ns::take(int, double const&)`. `ns::take(ns::Count, ns::Ratio const&)` can
report no symbol despite denoting the same C++ types. Retry the bare name and
use one of the supported spellings or its actual mangled name; do not call the
function to establish its existence. Supported `noexcept` queries must match
a nonthrowing declaration. This is AST spelling comparison, not arbitrary
semantic normalization of every possible alias or template spelling.

```text
namespace inspect { int calls = 0; int pick(int x) { ++calls; return x; } double pick(double x) { ++calls; return x; } struct Probe { int get() & { return 1; } int get() const & { return 2; } }; }
%syminfo inspect::pick
%syminfo 'inspect::pick(int)'
%info 'inspect::Probe::get() const &'
%syminfo 'inspect::pick(1)'
inspect::calls
```

The bare query shows both overloads; the typed queries select one declaration
each. The argument-expression query is rejected and `calls` remains 0. Neither
successful nor rejected inspection parses helper declarations or executes C++.
An unavailable address for an unemitted inline method does not mean its
declaration is absent. Use the actual reported mangled name for a later query;
prompt redefinitions can introduce internal version names, so do not guess it.

## Observe and change one call

Use a known baseline, one hook change at a time, and the same inputs. This
minimal experiment demonstrates hook order and restoration:

```text
int twice(int x) { return x * 2; }
twice(3)
%pre twice(int x) cpp`x += 1;`
%mock twice(int x) cpp`return x + 10;`
%post twice(int x) cpp`result *= 2;`
%hooks
twice(3)
%restore twice(int x)
twice(3)
```

Expected values: `6`, `28`, `6`. Each target has one pre, mock and post slot;
setting a slot replaces it. An empty body clears only that slot:

```text
%mock twice(int x) cpp``
```

`%restore signature` clears all three for the selected target; bare `%restore`
clears all targets, so prefer an exact signature in a session with unrelated hooks.

Name parameters to use them in a body; unnamed parameters become `arg0`, etc.
Method hooks use `self->member` or `this->member`; ordinary access rules apply.
`result` exists for non-void post hooks and can be modified. A post hook runs
after normal return, including a mock return, but not after an exception.
Global hooks affect all instances of the selected method.

JIT routing can change calls from previously compiled REPL callers and function
pointers. A global library hook affects JIT-generated calls; calls inside native
libraries and pointers returned by native code need separate coverage analysis.
Do not claim a library's internal callers changed merely because a prompt call
changed. For a supported **direct** caller filter:

```text
%pre app::target(int x) in app::caller(int x) cpp`x += 1;`
%restore app::target(int x) in app::caller(int x)
```

Verify backend/platform support and keep affected native code idle while native
call patches change. Check `%syminfo` for routing/original addresses and `%hooks`
for installed bodies. Native support depends on architecture/build flags and
call shape; indirect, inline, hidden or ambiguous calls may be unsupported.
Global and caller-specific hooks have different coverage. Hook limitations
also differ from `%define`: a nontrivial by-value parameter can prevent a hook
even though ordinary calls work. In that case, observe a supported boundary
such as a reference-taking caller, verify the rejected update preserved the
prior hook state, and restore only the target changed for the experiment. Do not
change the application's signature merely to make instrumentation install.

## Reload, redefinition, and failures

If `%help` includes `%reload`, load a scratch file with `%add`, record its
baseline, edit it, and run `%reload path.cpp` before the next probe. Changes may
also be detected before the next C++ input; this is not background execution.
No-argument `%reload` reloads all added files. Compare old and newly constructed
objects/callers explicitly. Changing a class does not migrate old instances,
and native/C-linkage ABI changes are not versioned this way.

Rejected reloads should preserve the last working definitions/macros; verify
that by rerunning the baseline. Reloaded initializers can have external effects
that cannot be undone. Repeated versions retain memory until process exit.

For a rejected input, follow with a known-good expression **and a use of an
earlier declaration**. Arithmetic alone does not prove namespace/overload lookup
survived. Recovery retains some preprocessor/nested declaration state; custom
headers first included in a rejected input may require a fresh session. Use
the matching `examples/include-recovery` and `examples/undo-crash` reproducers
when diagnosing those paths, rather than repeatedly adding the same header.

Current builds restore earlier namespace lookup after syntax errors and rejected
unresolved-symbol inputs. Check useful qualified source expressions, existing
objects and earlier compiled callers in the **same process**, then continue if
they remain correct. This also applies to namespaces loaded with `%add`.

```text
namespace recover { int value = 75; int read() { return value; } }
int retained() { return recover::read(); }
namespace recover { int broken = ; }
recover::value
retained()
namespace recover { int missing(); } int rejected = recover::missing();
recover::read()
retained()
```

The two rejected inputs produce their respective parse/undefined-symbol errors;
the four observations still print 75 without restarting. This validates retained
lookup/code, not complete rollback of preprocessing, nested declarations or
external effects. Keep the first successful header input separate from risky code.

Older installed builds can instead report the previously valid namespace as
undeclared while arithmetic, old callers or native metadata still work. If
that regression occurs, preserve the exact inputs, compare the installed
revision with the intended build, and replay the known-good setup in a fresh
process. A merged fix or historical wiki result does not prove installation
freshness. Neither `%clear` nor `%restore` repairs damaged compiler lookup state.
