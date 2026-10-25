# Methods and fields

From this directory, open the built REPL:

```sh
./run.sh
```

Set `CPP_REPL` to an absolute executable path to use another build.
Load the declarations (the methods need no implementations for inspection):

```text
%add records.hpp
%methods demo::Record
%fields demo::Record
```

The default fields output contains only the record's own member:

```text
demo::Record:
  private demo::Record::own_ : int
```

Try all access filters and parent traversal:

```text
%methods demo::Record access public
%fields demo::Record access private include-parents true
%methods demo::Record access protected include-parents true
%fields demo::Record access all include-parents true
%methods demo::Record include-parents true
%fields demo::Choice
%methods demo::Choice
```

Parent groups appear in order: `demo::Record`, `demo::Left`, `demo::Base`,
`demo::Right`. The shared base appears once. Filtering uses declared access:
`demo::Base::value` remains public in this listing through the private `Left`
inheritance. Static data members are included in fields. Methods preserve
overload types and qualifiers. Implicit compiler-generated methods are omitted.
`access all` and `include-parents false` are the defaults.

Replay the complete demonstration with `./run.sh < demo.repl`, or run the
checked version with `./verify.sh`. Both command names, record names, option
keywords and option values support Tab completion.
