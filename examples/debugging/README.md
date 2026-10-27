# Step through JIT code with LLDB

From the repository root, run `./build/cpp-repl --debug`.

1. At `(lldb)`, enter `breakpoint set -f functions.cpp -l 7`, then `run`.
   The breakpoint is pending until the JIT loads the function.
2. At `cpp-repl>`, enter `%add examples/debugging/functions.cpp`, then
   `debug_twice(21)`. LLDB stops before computing `doubled`.
3. Enter `next`, then `frame variable doubled`: its value is `42`.
4. Enter `step` once to enter `debug_increment` at line 2; its hook dispatcher is
   crossed automatically.
5. Enter `frame variable value` (`42`), `next`, then `frame variable result` (`43`).
   `expression -- value + 1` evaluates an expression in the selected frame.
6. Enter `bt` for the stack. One `finish` command returns through the dispatcher
   to `debug_twice`. `continue` completes the call and prints `(int) 43`.
7. At `cpp-repl>`, `%quit` ends the program; at `(lldb)`, `quit` exits LLDB.

For prompt definitions, LLDB displays saved `input-N.cpp` files. Use a name
breakpoint before `run`, or interrupt with Ctrl+C to set a breakpoint after
defining a function. A name breakpoint can stop at both its dispatcher and body.
The temporary prompt sources remain available until normal interpreter shutdown.

To use an existing debugger, launch `./build/cpp-repl --debug-info` under LLDB
after `settings set plugin.jit-loader.gdb.enable on` and
`command script import /path/to/cpp_repl_lldb.py`. Automatic skipping requires
LLDB's Python scripting support; CMake installs the companion beside cpp-repl.
Explicit breakpoints, watchpoints, signals and `stepi` still stop normally.
Native libraries loaded with
`%lib` need their own debug symbols; build them with `-g -O0` for readable locals.
