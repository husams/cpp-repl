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


def final_repaint(output, marker):
    return next((line for line in reversed(plain(output).splitlines()) if marker in line), "")


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
        ("embedded", b"committed_add(1, 2) + 20;\r", True),
        ("multiple", b"committed_add(1, 2) + committed_add(3, 4) + 20;\r", True),
        ("nested", b"committed_add(1, committed_add(2, 3)) + 20;\r", True),
        ("fake_calls", b'committed_add(1, 2) + sizeof("fake(7, 8)") + 20; // fake(3, 4)\r', True),
        ("method_definition", b"struct HintBox { int add(int a, int b) { return a + b; } };\r", True),
        ("method", b"HintBox{}.add(5, 6) + 20;\r", True),
        ("disable_hints", b"%set keep_hints = false\r", True),
        ("embedded_hints_disabled", b"committed_add(1, 2) + 20;\r", True),
        ("enable_hints", b"%set keep_hints = true\r", True),
        ("cursor_input", b"committed_add(1, 2) + 20;"),
        ("cursor_first_arg", b"\x01" + b"\x1b[C" * 15),
        ("cursor_type", b"9"),
        ("cursor_submit", b"\x05\r", True),
        ("cursor_effect", b"commit_calls\r", True),
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
    for key, marker in (("embedded", "committed_add("), ("multiple", "committed_add("),
                        ("nested", "committed_add("), ("method", "HintBox{}.add("),
                        ("fake_calls", "committed_add(")):
        rendered = final_repaint(out[key], marker)
        check(key + " keeps parameter names and types on embedded calls",
              "a: " in rendered and "b: " in rendered and "(int)" in rendered and
              b"\x1b[0;90m" in out[key])
        check(key + " omits return annotations before trailing expression code",
              "-> int" not in rendered)
    check("multiple embedded calls both retain annotations",
          final_repaint(out["multiple"], "committed_add(").count("a: ") >= 2 and
          final_repaint(out["multiple"], "committed_add(").count("b: ") >= 2)
    check("comments and strings do not create fake call annotations",
          "a: 7" not in final_repaint(out["fake_calls"], "committed_add(") and
          "a: 3" not in final_repaint(out["fake_calls"], "committed_add("))
    check("keep_hints=false hides embedded committed hints",
          "a: " not in plain(out["embedded_hints_disabled"]) and
          "b: " not in plain(out["embedded_hints_disabled"]))
    for key, value in (("embedded", "(int) 23"), ("multiple", "(int) 30"),
                       ("nested", "(int) 26"), ("method", "(int) 31"),
                       ("fake_calls", "(unsigned long) 34")):
        check(key + " evaluates once with its expected result", value in plain(out[key]))
    check("typing in an earlier active argument keeps its annotations",
          "a: 19 (int)" in plain(out["cursor_type"]) and "b: 2 (int)" in plain(out["cursor_type"]))
    check("moving the cursor and editing does not replay the call",
          "(int) 12" in plain(out["cursor_effect"]))
    check("session exits normally", status == 0)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
