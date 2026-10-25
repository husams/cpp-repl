# Template callable examples

These cases work with ordinary C++ definitions entered in the REPL. Direct
`%define` signatures for these template targets are currently unsupported.
The `%define` signature parser cannot handle their template argument lists;
removing those lists or using a type alias does not resolve the target either.

| Example | Definition supplied in the REPL | Expected text |
| --- | --- | --- |
| `class-template-instance` | Generic `Person<T>::getName()`, instantiated for `int` and `long` | `Person 7`, `Person 9` |
| `specialized-method` | Explicit `Person<int>::getName()` specialization, plus the generic method for other types | `specialized int 7`, `generic` |
| `function-template-instance` | Generic `describe<T>()`, explicit `int` instantiation and implicit `long` instantiation | `value 7`, `value 9` |

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

To reproduce the current `%define` rejection separately:

```sh
./run.sh class-template-instance --probe-define
./run.sh specialized-method --probe-define
./run.sh function-template-instance --probe-define
```

Those probe runs deliberately print errors and exit. They do not load the
working definitions. Their inputs are saved as `<case>/unsupported.repl`.
For an explicit method specialization, supply the `template<>` definition
before the first call that would instantiate that method.

The runner uses this worktree's built `cpp-repl` with hidden visibility. Set
`CPP_REPL_BIN` to use another build. Template `%mock` support is separate from
these definition examples.
