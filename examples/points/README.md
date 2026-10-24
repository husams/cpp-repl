# Private-caller hook example

## macOS ARM64

The Mac example is built locally. Start it with:

```sh
cd /Users/husam/workspace/replxx-example/examples/points
./start.sh
```

Load the Mac library with `%lib ./build/libpoints.dylib`, then use the same
header, object, hook, and restore commands shown below for Linux. To replay
the complete Mac demonstration automatically, run `./start.sh < demo-macos.repl`.

To rebuild the disposable Mac example if desired:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Linux x86_64

The precompiled `build/libpoints.so` contains `add`, private `Point::inc`,
`Point::run`, and `Point::double_it`. `run` calls `inc`; `double_it` calls `add`
independently. Every implementation lives in the library, outside the header.

The example is built on `linux-dev` and available at `~/replxx-points-example`.
Start an interactive REPL from your own terminal:

```sh
ssh -t linux-dev '~/replxx-points-example/start.sh'
```

Paste these inputs into the REPL (without any prompt prefix):

```text
%lib ./build/libpoints.so
#include "point.hpp"
#include <iostream>
Point p;
p.run(3)
p.double_it(3)
```

The original results are 4 and 6. Install the hooks:

```text
%pre add(int a, int b) in Point::inc(int) cpp`
  std::cout << "pre: add(" << a << ", " << b << ")\n";
`
%mock add(int a, int b) in Point::inc(int) cpp`
  return a + b + 100;
`
%post add(int, int) in Point::inc(int) cpp`
  std::cout << "post: result=" << result << "\n";
  result += 10;
`
p.run(3)
p.double_it(3)
add(3, 1)
%hooks
```

`p.run(3)` prints `pre: add(3, 1)`, then `post: result=104`, and returns 114.
`p.double_it(3)` returns 6 and `add(3, 1)` returns 4; neither prints hook messages.
`%hooks` lists all three hook bodies and their caller filter.

Restore this pair, then call again:

```text
%restore add(int, int) in Point::inc(int)
p.run(3)
p.double_it(3)
%hooks
%quit
```

The results are 4 and 6, without hook messages; `%hooks` reports an empty session.
Bare `%restore` also works and clears all hooks in the session.

To replay the complete demonstration automatically:

```sh
ssh linux-dev 'cd ~/replxx-points-example && ./start.sh < demo.repl'
```

The library only needs building once. Installing or restoring hooks changes
memory in the running REPL process, without rebuilding or modifying the library
file. `Point::inc` remains private. Symbol addresses and native call sites must
remain available; the build keeps calls from being inlined for this example.

To rebuild this disposable example on Linux if desired:

```sh
cd ~/replxx-points-example
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`start.sh` uses this project's `build/cpp-repl` on macOS or
`build-linux/cpp-repl` on Linux. Set `CPP_REPL` to an absolute executable path
to use another build.
