# Template callable examples

All three cases use `%define` to implement concrete template instances in the
JIT. Supply the template arguments in the target name; the REPL selects the
matching declaration and emits the required C++ specialization automatically.
Return types may be omitted, as with other `%define` signatures.

| Example | Definition supplied in the REPL | Expected text |
| --- | --- | --- |
| `class-template-instance` | `%define` for `Person<int>::getName()` and `Person<long>::getName()` | `Person 7`, `Person 9` |
| `specialized-method` | `%define` for the header-declared `Person<int>::getName()` specialization and a separate `double` instance | `specialized int 7`, `double instance` |
| `function-template-instance` | `%define` for `describe<int>(int)` and `describe<long>(long)` | `value 7`, `value 9` |

From this directory, run all working examples and exit:

```sh
./run.sh all
```

Each case prints its string result and `(bool) true`. To leave one open for
manual testing, run its name, for example:

```sh
./run.sh specialized-method
```

Its header is in `<case>/include/api.hpp`; its working definitions are in
`<case>/.cpp-repl/01-example.repl`. Exit with `%quit`.

To run just one `%define` and check its result in a fresh session:

```sh
./run.sh class-template-instance --probe-define
./run.sh specialized-method --probe-define
./run.sh function-template-instance --probe-define
```

These runs print `(bool) true` and exit. Their inputs are saved as
`<case>/define-only.repl`. No manual `template<>` prefix is needed in `%define`.
Define a concrete instance before its first call; already-defined targets and
dependent primary-template patterns cannot be replaced by `%define`.

The runner uses this worktree's built `cpp-repl` with hidden visibility. Set
`CPP_REPL_BIN` to use another build. Template `%mock` support is separate from
these definition examples.
