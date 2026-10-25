# String-returning method

Run `./run.sh` from this directory. It uses this worktree's built `cpp-repl`;
set `CPP_REPL_BIN` to use another build.

The startup script includes `include/person.hpp`, supplies `Person::getName()`
through `%define`, creates `p`, and calls the method with and without a trailing
semicolon. Hidden visibility reproduces the original `__ci_newtag` failure on
the unfixed build.

The fixed build prints a string object, `(bool) true`, and
`(const char *) "Sample"`, then leaves the REPL open for manual testing.
Try `p.getName()` or `p.getName() == "Sample"`; exit with `%quit`.
