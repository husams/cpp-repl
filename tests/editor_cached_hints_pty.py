"""Cached call hints follow user-visible edits to earlier and active calls."""
import os
from pathlib import Path
import runpy
import sys
import tempfile


pty_helpers = runpy.run_path(str(Path(__file__).with_name("editor_large_context_benchmark.py")))
PtySession = pty_helpers["PtySession"]
visible = pty_helpers["visible"]
binary = os.path.abspath(sys.argv[1])
RIGHT = b"\x1b[C"


def rights(count):
    return RIGHT * count


with tempfile.TemporaryDirectory() as cwd:
    repl = PtySession(binary, cwd)
    out = {}

    def send_until(name, keys, needle, timeout=15):
        # Each wait consumes only output produced after this edit. Drain before
        # the next edit so a queued repaint cannot satisfy its assertion.
        repl.drain(quiet=0.03)
        repl.send(keys)
        out[name] = repl.read_until(needle, timeout=timeout)
        repl.drain(quiet=0.03)
        return out[name]

    try:
        out["startup"] = repl.read_until(b"cpp-repl> ")
        send_until("include", b"#include <string>\r", b"cpp-repl> ")
        send_until("definitions", b'int int_probe(int first) { return 11; } '
                   b'int str_probe(const char *text) { return 22; } '
                   b'int enter_cache_probe(int required, int optional = 5) '
                   b'{ return required + optional; } '
                   b'int dbl_probe(double real) { return 33; }\r', b"cpp-repl> ")

        line = b"int_probe(7) + int_probe(2) + dbl_probe(3.0) + 0"
        # Wait for the first hint on this line; a separate empty edit could
        # drain the only queued repaint before it is observed.
        send_until("initial_line", line, b"first: ")

        # Replace the first completed call with an equal-name-length callee
        # whose parameter label and type make stale hints visible.
        send_until("string_call", b"\x01" + b"\x04" * 12 + b'str_probe("changed")',
                   b'text: "changed"')

        # Rename the next call to the double function and change its argument.
        # All three function identifiers have equal length, so only the edited
        # source prefix changes here.
        send_until("double_call", b"\x01" + rights(23) + b"\x04" * 9 + b"dbl_probe" +
                   b"\x01" + rights(34) + b"\x7f2.5", b"real: 2.5 (double)")
        send_until("prefix_insert", b"\x01" + b"0 + ", b"real: 2.5 (double)")
        send_until("prefix_delete", b"\x01" + b"\x04" * 4, b"real: 2.5 (double)")
        send_until("cursor_context", b"\x01" + rights(34), b"real: 2.5 (double)")
        send_until("suffix_edit", b"\x05\x7f1", b"real: 2.5 (double)")

        send_until("submit", b"\r", b"cpp-repl> ")
        send_until("cancel_stale_line", b"str_probe(", b"str_probe(")
        send_until("cancel", b"\x03", b"cpp-repl> ")
        send_until("new_prompt", b"int_probe(4)\r", b"cpp-repl> ")
        send_until("redefine", b"int int_probe(int first) { return 44; }\r", b"cpp-repl> ")
        send_until("after_redefinition", b"int_probe(1)\r", b"cpp-repl> ")

        # The first Enter removes the trailing optional slot and leaves the
        # required argument gap. The second Enter arrives before an idle hint
        # refresh, so Enter must retain enough metadata to revisit this call.
        repl.drain(quiet=0.03)
        repl.send(b"enter_cache_probe(, ) + 1\r\r7\r")
        out["rapid_enter_after_trim"] = repl.read_until(b"cpp-repl> ", timeout=15)
        repl.drain(quiet=0.03)
    finally:
        repl.close()

    failures = []

    def check(label, ok):
        print(("PASS " if ok else "FAIL ") + label)
        if not ok:
            failures.append(label)

    check("editing an earlier call refreshes its parameter name and value",
          b'text: "changed"' in visible(out["string_call"]))
    check("editing the next call refreshes its double argument hint",
          b"real: 2.5 (double)" in visible(out["double_call"]))
    check("inserting and deleting text before calls keeps each hint attached",
          b"real: 2.5 (double)" in visible(out["prefix_insert"]) and
          b"real: 2.5 (double)" in visible(out["prefix_delete"]))
    check("cursor context inside an edited call shows its current hint",
          b"real: 2.5 (double)" in visible(out["cursor_context"]))
    check("editing after completed calls preserves hints and executes edited source",
          b"real: 2.5 (double)" in visible(out["suffix_edit"]) and
          b"(int) 89" in visible(out["submit"]) and b"error:" not in visible(out["submit"]))
    check("Ctrl+C clears the old line before a fresh prompt",
          b"^C" in out["cancel"] and b"(int) 11" in visible(out["new_prompt"]))
    check("a redefined function is used at the next prompt",
          b"(int) 44" in visible(out["after_redefinition"]))
    check("rapid second Enter after trimming keeps the required-argument call active",
          b"(int) 13" in visible(out["rapid_enter_after_trim"]) and
          b"error:" not in visible(out["rapid_enter_after_trim"]))
    check("REPL exits normally", repl.closed)
    if failures:
        for key, value in out.items():
            print(f"{key}: {visible(value)}")
    sys.exit(bool(failures))
