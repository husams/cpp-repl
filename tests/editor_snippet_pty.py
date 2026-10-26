#!/usr/bin/env python3
"""Verify display-only argument hints and snippet navigation in the real editor."""
import os
import re
from pathlib import Path
import runpy
import sys
import tempfile

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory() as cwd:
    out, status = session(binary, cwd, [
        ("definitions", b"int snippet_effects = 0; int snippet_add(int x, int y) { ++snippet_effects; return x + y; } int snippet_one(int value) { return value + 1; } int snippet_default(int x, int y = 5) { return x + y; } int default_gap(int a = 1, int b = 2) { return a + b; } int req_gap(int a, int b) { return a + b; } int req_tri(int a, int b, int c) { return a + b + c; } int snippet_zero() { return 7; } int snippet_text(const char* text, int value) { return value; } struct SnippetObject { int method(int amount, int multiplier) const { return 10 + amount * multiplier; } int zero() const { return 7; } }; SnippetObject snippet_obj; template<class T, class U> int snippet_template(T, U) { return 20; }\r", True),
        ("overload_definitions", b"int snippet_over(int first) { return first; } int snippet_over(int first, int second) { return first + second; }\r", True),
        ("variable_definitions", b"int snippet_x = 20; int snippet_x_other = 3;\r", True),
        ("empty_call", b"snippet_ad\t"),
        ("empty_enter", b"\r"),
        ("cancel_empty", b"\x03", True),
        ("empty_effects", b"snippet_effects\r", True),
        ("empty_exit_enter", b"snippet_ad\t\t\t\r"),
        ("cancel_empty_exit", b"\x03", True),
        ("empty_exit_effects", b"snippet_effects\r", True),
        ("function_hint", b"auto snippet_result = snippet_ad\t"),
        ("function_fill", b"20\t22\t;\r", True),
        ("function_value", b"snippet_result\r", True),
        ("method_hint", b"snippet_obj.met\t"),
        ("method_value", b"4\t8\t\r", True),
        ("manual_hint", b"snippet_add("),
        ("manual_value", b"20\t22\r", True),
        ("skip_empty", b"snippet_ad\t\t22\x1b[Z20\x05\r", True),
        ("default_hint", b"snippet_def\t"),
        ("default_value", b"37\r", True),
        ("default_gap", b"default_ga\t\t2\r"),
        ("default_gap_fill", b"1\r", True),
        ("default_gap_empty_call", b"default_gap()\r", True),
        ("default_gap_one_argument", b"default_gap(1)\r", True),
        ("required_embedded_gap", b"req_ga\t\t2\x05+20;\r"),
        ("required_embedded_fill", b"1\r", True),
        ("required_embedded_multiple", b"req_gap(, 2)+snippet_one(1)\r"),
        ("required_embedded_multiple_fill", b"1\r", True),
        ("required_embedded_three_args", b"req_tri(1,2)+20;\r"),
        ("required_embedded_three_args_fill", b"3\r", True),
        ("default_tab_value", b"snippet_def\t37\t\r", True),
        ("default_tab_exit_value", b"snippet_def\t37\t\t\r", True),
        ("zero_hint", b"snippet_zer\t"),
        ("zero_value", b"\r", True),
        ("zero_method", b"snippet_obj.zer\t\r", True),
        ("nested_value", b"snippet_ad\tsnippet_one(19)\t22\r", True),
        ("nested_completion", b"snippet_ad\tsnippet_on\t19\t\t22\r", True),
        ("overload_hint", b"snippet_ove\x0e\x0e"),
        ("overload_value", b"20\t22\r", True),
        ("nested_overload", b"snippet_ove\x0e\x0esnippet_on\x0e19\t\t22\r", True),
        ("variable_argument", b"snippet_ad\tsnippet_x\t22\r", True),
        ("string_value", b'snippet_tex\t"a,b"\t42\r', True),
        ("template_value", b"snippet_ad\tsnippet_template<int, int>(1, 2)\t22\r", True),
        ("unicode_hint", "/* café */ snippet_add(".encode()),
        ("unicode_value", b"20\t22\r", True),
        ("ordinary_comma", b"snippet_ad\t20, 22\r", True),
        ("quit", b"%quit\r"),
    ])
    for key in ("function_hint", "manual_hint"):
        text = plain(out[key])
        check(key + " shows names and types inside the call",
              "x:" in text and "y:" in text and "(int)" in text and "-> int" in text)
        check(key + " uses the existing gray hint color", b"\x1b[0;90m" in out[key])
        check(key + " puts parameters inside and the return type outside parentheses",
              re.search(r"snippet_add\(x:\s+\(int\),\s*y:\s+\(int\)\)\s*-> int", text) is not None)
        check(key + " leaves argument slots blank", "<placeholder>" not in text)
    check("method hints retain parameter names and types",
          "amount:" in plain(out["method_hint"]) and "multiplier:" in plain(out["method_hint"]))
    check("selected overload retains both parameter slots",
          "first:" in plain(out["overload_hint"]) and "second:" in plain(out["overload_hint"]))
    check("Enter cannot execute required empty placeholders",
          "error:" not in plain(out["empty_enter"]) and "(int) 0" in plain(out["empty_effects"]))
    check("leaving the call cannot bypass required empty placeholders",
          "error:" not in plain(out["empty_exit_enter"]) and "(int) 0" in plain(out["empty_exit_effects"]))
    check("inline hints replace the standalone signature hint",
          "[argument " not in plain(out["function_hint"]))
    for key in ("function_fill", "method_value", "manual_value", "skip_empty", "default_value", "default_tab_value", "default_tab_exit_value",
                "nested_value", "nested_completion", "overload_value", "nested_overload", "variable_argument",
                "string_value", "template_value", "unicode_value", "ordinary_comma"):
        check(key + " inserts executable C++ without hint text", "error:" not in plain(out[key]))
    for key in ("function_value", "method_value", "manual_value", "skip_empty", "default_value", "default_tab_value", "default_tab_exit_value",
                "nested_value", "nested_completion", "overload_value", "nested_overload", "variable_argument",
                "string_value", "template_value", "unicode_value", "ordinary_comma"):
        check(key + " evaluates the intended arguments", "(int) 42" in plain(out[key]))
    check("Enter returns to a nontrailing default gap instead of submitting it",
          "error:" not in plain(out["default_gap"]) and
          "default_gap(, 2)" in plain(out["default_gap"]) and
          "(int) 3" in plain(out["default_gap_fill"]))
    check("trailing defaults remain optional in empty and one-argument calls",
          "error:" not in plain(out["default_gap_empty_call"]) and
          "(int) 3" in plain(out["default_gap_empty_call"]) and
          "error:" not in plain(out["default_gap_one_argument"]) and
          "(int) 3" in plain(out["default_gap_one_argument"]))
    check("Enter returns to a required gap inside a larger expression",
          "error:" not in plain(out["required_embedded_gap"]) and
          "req_gap(, 2)+20;" in plain(out["required_embedded_gap"]) and
          "(int) 23" in plain(out["required_embedded_fill"]))
    check("a later valid call does not hide an earlier required gap",
          "error:" not in plain(out["required_embedded_multiple"]) and
          "(int) 5" in plain(out["required_embedded_multiple_fill"]))
    check("Enter restores a missing third argument in an embedded call",
          "error:" not in plain(out["required_embedded_three_args"]) and
          "(int) 26" in plain(out["required_embedded_three_args_fill"]))
    for key in ("zero_value", "zero_method"):
        check(key + " has no argument placeholders", "(int) 7" in plain(out[key]))
    check("zero-argument calls retain return type hints",
          "snippet_zero() -> int" in plain(out["zero_hint"]) and
          "<placeholder>" not in plain(out["zero_hint"]))
    check("snippet session exits normally", status == 0)
    history = Path(cwd) / ".replxx_history"
    if history.exists():
        saved = history.read_text()
        check("display labels stay out of history",
              "<placeholder>" not in saved and "<value>" not in saved and "x:" not in saved)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
