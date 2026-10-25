# Iterative C++ development

- [x] Declaration shadowing: redefine classes, functions, variables, aliases, enums and templates at the same prompt; preserve older objects and compiled references.
- [x] Reload: automatically reread edited added files and user headers before the next C++ input, with explicit `%reload [paths ...]`, current compile options, and rollback after errors.
- [x] Verify real prompt sessions, regressions, and full test suite; document behavior and limitations.

Delivered in the primary checkout without overwriting its pre-existing changes. CTK identified the command parser, session dispatcher, interpreter parse/execute path, and source tracking.

Validation on macOS / LLVM 22.1.8: **203/203 CTest tests pass**, including terminal smoke tests and 12 iterative-development regressions. A real executable session verified class redefinition, preservation of old objects, automatic source reload and explicit `%reload`. Independent review found two rollback issues (macros and include-once state); both were fixed and covered by regressions.

Existing objects and compiled callers retain their original definitions. Old generations remain allocated until session shutdown. Explicit template specializations, C linkage and precompiled ABI changes remain subject to C++ restrictions. The rebuilt REPL must be started once to enable the new features.
