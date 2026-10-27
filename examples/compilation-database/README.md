# Using compile_commands.json

Run the example with the repository build:

```sh
./run.sh < demo.repl
./verify.sh
```

For another executable, use `CPP_REPL=/absolute/path/to/cpp-repl ./run.sh`
or `./verify.sh /absolute/path/to/cpp-repl`. The verifier runs a temporary copy
and prints the directory containing its complete logs.

The checked-in database has two entries. Its `directory` is `.`; cpp-repl
resolves this relative to the database location, so the example can be copied
without editing machine-specific paths. Both entries include `-Iinclude`.

| Entry | Macro | Standard | Function result |
| --- | --- | --- | --- |
| `src/alpha.cpp` | `DEMO_VALUE=11` | C++17 | `alpha_value() == 111` |
| `src/beta.cpp` | `DEMO_VALUE=22` | C++20 | `beta_value() == 122` |

## 1. An added file uses its own entry

```text
%compile-opts src/beta.cpp
%add src/beta.cpp
beta_value()
%compile-opts src/alpha.cpp
%add src/alpha.cpp
alpha_value()
```

The values are `122` and `111`. Each source checks its macro and C++ standard
with `static_assert`, and includes `bonus.hpp` through the database's relative
include path. Loading beta first proves the REPL does not simply use the first
entry for every file. Adding `other/no_defaults.cpp` also checks that these
per-file flags do not automatically become session defaults.

## 2. A file without an entry borrows one from its directory

```text
%compile-opts src/unlisted.cpp
%add src/unlisted.cpp
unlisted_value()
%compile-opts src/beta.hpp
%add src/beta.hpp
header_value()
```

Neither file is listed in the database. `unlisted.cpp` borrows `alpha.cpp`'s
flags by alphabetical order and returns `112`. The header `beta.hpp` borrows
`beta.cpp`'s flags because their filename stems match, and returns `123`.
The displayed database command identifies the selected donor.

This fallback applies to a **missing database entry**, not a missing source
file. The verifier separately checks that `%compile-opts src/missing.cpp` can
describe fallback options, while `%add src/missing.cpp` reports `no such file`.

## 3. Choose any entry as the session defaults

```text
%use-compile-opts src/beta.cpp
%compile-opts
DEMO_VALUE
__cplusplus
#include <prompt.hpp>
DEMO_VALUE + prompt_bonus
%add other/use_beta.cpp
default_beta_value()
%use-compile-opts src/alpha.cpp
%compile-opts
DEMO_VALUE
__cplusplus
%add other/use_alpha.cpp
default_alpha_value()
```

Beta supplies `22`, `202002`, and a result of `122`. Switching to alpha replaces
those defaults with `11`, `201703`, and a result of `111`. Both prompt input
and files in `other/` (which has no database entries) receive the chosen flags.
The first `prompt.hpp` include checks that the reference also supplies relative
include paths. `%use-compile-opts` selects settings without adding the reference
source. Existing compiled functions keep their original results after switching.

`demo.repl` checks compile settings with `static_assert` and checks function
results at runtime before printing `PASS` markers; `verify.sh` rejects any
unexpected compiler diagnostic even if the REPL exits with status zero.
