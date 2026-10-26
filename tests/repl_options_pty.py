#!/usr/bin/env python3
"""Exercise live option queries, assignments, completion and editor behavior."""
import os
from pathlib import Path
import runpy
import sys
import tempfile

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])


def final_call(output):
    return plain(output).rsplit("option_add(", 1)[-1].splitlines()[0]


with tempfile.TemporaryDirectory() as cwd:
    out, status = session(binary, cwd, [
        ("print_default", b"$print_values\r", True),
        ("hint_default", b"$keep_hints\r", True),
        ("definition", b"int option_calls = 0; int option_add(int a, int b) { ++option_calls; return a + b; }\r", True),
        ("blank_slots", b"option_ad\t"),
        ("default_call", b"10\t20\t;\r", True),
        ("mute", b"%set print_values = false\r", True),
        ("muted_query", b"$print_values\r", True),
        ("muted_call", b"option_add(10, 20);\r", True),
        ("muted_expression", b"option_calls\r", True),
        ("hide_hints", b"%set keep_hints = false\r", True),
        ("hidden_query", b"$keep_hints\r", True),
        ("hidden_call", b"option_add(10, 20);\r", True),
        ("enable_complete", b"%set print_v\t = t\t\r", True),
        ("counter", b"option_calls\r", True),
        ("printed_hidden", b"option_add(10, 20);\r", True),
        ("enable_hints", b"%set keep_hints=true\r", True),
        ("restored_call", b"option_add(10, 20);\r", True),
        ("query_complete", b"$keep_h\t\r", True),
        ("bad_value", b"%set print_values = maybe\r", True),
        ("unchanged_query", b"$print_values\r", True),
        ("unknown", b"$no_such_option\r", True),
        ("quit", b"%quit\r"),
    ])
    check("both options default to true",
          "print_values = true" in plain(out["print_default"]) and
          "keep_hints = true" in plain(out["hint_default"]))
    blank = plain(out["blank_slots"])
    check("argument slots are blank and gray",
          "a:  (int)" in blank and "b:  (int)" in blank and
          "<placeholder>" not in blank and b"\x1b[0;90m" in out["blank_slots"])
    check("semicolon calls print values by default", "(int) 30" in plain(out["default_call"]))
    check("default submitted calls retain hints", "a: 10 (int)" in final_call(out["default_call"]))
    check("print_values can be queried when false", "print_values = false" in plain(out["muted_query"]))
    check("muting suppresses all automatic values",
          "(int) 30" not in plain(out["muted_call"]) and "(int) 2" not in plain(out["muted_expression"]))
    check("muting does not disable hint retention", "a: 10 (int)" in final_call(out["muted_call"]))
    check("keep_hints can be queried when false", "keep_hints = false" in plain(out["hidden_query"]))
    check("keep_hints false removes only committed annotations", "a:" not in final_call(out["hidden_call"]))
    check("option and value completion update the option", "print_values = true" in plain(out["enable_complete"]))
    check("muted expressions still run once", "(int) 3" in plain(out["counter"]))
    check("printing works independently of hint retention",
          "(int) 30" in plain(out["printed_hidden"]) and "a:" not in final_call(out["printed_hidden"]))
    check("hint retention can be restored", "a: 10 (int)" in final_call(out["restored_call"]))
    check("query completion works", "keep_hints = true" in plain(out["query_complete"]))
    check("invalid assignments do not change state",
          "error:" in plain(out["bad_value"]) and "print_values = true" in plain(out["unchanged_query"]))
    check("unknown options report a REPL error",
          "unknown option" in plain(out["unknown"]) and "Parsing failed" not in plain(out["unknown"]))
    saved = (Path(cwd) / ".replxx_history").read_text()
    check("history stores no display decorations", "<placeholder>" not in saved and "a:" not in saved)
    check("session exits normally", status == 0)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
