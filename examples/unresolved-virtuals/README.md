# Manual tests: non-pure virtual method and virtual override

Run this example from the isolated worktree, which has the new REPL build:

```sh
cd /Users/husam/.Codex/worktrees/replxx-example/codex/virtual-definitions/examples/unresolved-virtuals
./configure.sh
./run.sh
```

Startup reads `include/api.hpp`, defines both missing methods, then loads the
single supplied `libvirtual_consumer` library. `run.sh` selects this checkout's
build automatically; `CPP_REPL=/absolute/path/to/cpp-repl` overrides it.

## Case 1: non-pure virtual method

`Service` declares `virtual int compute(int) const;` without a definition.
Startup supplies a real member definition, with access to its private state:

```text
%define virtual_demo::Service::compute(int x) const cpp`
return this->seed_ + x;
`
```

At the prompt, test an already compiled native caller:

```text
virtual_demo::non_pure_probe(2)
```

Expected: `(int) 42`. The library constructs `Service(40)` and calls through a
`const Service&`. Clang emits the previously missing vtable and RTTI when the
REPL defines the method.

## Case 2: concrete virtual override

`API::compute` is pure virtual. `Implementation` declares a concrete
`int compute(int) const override;` whose implementation is missing.
Startup defines the derived override:

```text
%define virtual_demo::Implementation::compute(int x) const cpp`
return this->bias_ + x * 2;
`
```

At the prompt:

```text
virtual_demo::override_probe(3)
virtual_demo::rtti_probe()
virtual_demo::direct_probe()
```

Expected: `(int) 106`, `(int) 1`, `(int) 1`. Native code constructs
`Implementation(100)` and calls through its secondary `API` base. This exercises
the compiler-generated `this` adjustment thunk, along with `dynamic_cast`,
`typeid`, and explicitly qualified native calls to both supplied methods.

The definitions have already run during startup: enter the probe calls, rather
than repeating `%define` for a method which is now defined.

## Try different bodies

Edit `.cpp-repl/02-non-pure.repl` or `.cpp-repl/03-override.repl` and restart
`./run.sh`. The original consumer library needs no rebuild or relink.
For example, return `seed_ + x + 1000` in the first body; the native probe will
then return 1042. Restore the original body before running verification.

## Automated proof

```sh
./verify.sh
```

The verifier checks exactly one supplied library, undefined method/vtable
symbols in the final file, standalone loading failure without definitions,
native virtual and direct dispatch after startup, RTTI, and identical SHA-256
hashes before/after. Saved transcripts are in `evidence/`.

`%define` supplies missing implementations; it does not replace an existing
implementation, add an override absent from the class declaration, or make a
pure virtual declaration concrete. Define a concrete derived override instead.
Supply required dependent virtual methods before the key method that emits a
class's vtable. Undefined dependencies fail explicitly and can be retried.
