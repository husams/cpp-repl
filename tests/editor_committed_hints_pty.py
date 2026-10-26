#!/usr/bin/env python3
"""Keep inline annotations on the committed terminal line, never in C++ input."""
import os
from pathlib import Path
import runpy
import sys
import tempfile

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])


def committed_line(output):
    # Replxx repaints the input after the existing prompt without rewriting it.
    # Inspect the final call repaint, not an earlier idle hint in the transcript.
    text = plain(output)
    return text.rsplit("committed_add(", 1)[-1].splitlines()[0]


with tempfile.TemporaryDirectory() as cwd:
    out, status = session(binary, cwd, [
        ("definition", b"int commit_calls = 0; int committed_add(int a, int b) { ++commit_calls; return a + b; }\r", True),
        ("mute", b"%set print_values = false\r", True),
        ("semicolon", b"committed_add(10, 20);\r", True),
        ("unmute", b"%set print_values = true\r", True),
        ("semicolon_effect", b"commit_calls\r", True),
        ("expression", b"committed_add(10, 20)\r", True),
        ("expression_effect", b"commit_calls\r", True),
        ("completed", b"committed_ad\t10\t20\t;\r", True),
        ("completed_effect", b"commit_calls\r", True),
        ("commented", b"committed_add(10, 20); // )\r", True),
        ("commented_effect", b"commit_calls\r", True),
        ("quit", b"%quit\r"),
    ])
    for key in ("semicolon", "expression", "completed", "commented"):
        line = committed_line(out[key])
        check(key + " retains gray names, types and return annotation on submission",
              "a: 10 (int)" in line and "b: 20 (int)" in line and "-> int" in line and
              b"\x1b[0;90m" in out[key])
        check(key + " executes clean C++", "error:" not in plain(out[key]))
    check("semicolon suppresses the value", "(int) 30" not in plain(out["semicolon"]))
    check("expression prints its value", "(int) 30" in plain(out["expression"]))
    for index, key in enumerate(("semicolon_effect", "expression_effect", "completed_effect", "commented_effect"), 1):
        check(key + " runs exactly once", f"(int) {index}" in plain(out[key]))
    saved = (Path(cwd) / ".replxx_history").read_text()
    check("history contains source without annotations", "a:" not in saved and "-> int" not in saved)
    check("session exits normally", status == 0)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
