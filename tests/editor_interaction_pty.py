#!/usr/bin/env python3
"""Exercise completion edits and delimiter pairing through the real terminal."""
import os
from pathlib import Path
import runpy
import sys
import tempfile

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory() as cwd:
    headers = Path(cwd) / "headers"
    (headers / "nested").mkdir(parents=True)
    (headers / "nested" / "beta_header.hpp").write_text("#pragma once\nint editor_header_value = 73;\n")
    (headers / "system_header.hpp").write_text("#pragma once\nint editor_system_value = 74;\n")
    existing = b"editor_obj.method(5)"
    back_to_name = b"\x1b[D" * (len(existing) - len(b"editor_obj.met"))
    include_line = b'#include "nested/beta_header.hpp"'
    back_to_filename = b"\x1b[D" * (len(include_line) - len(b'#include "nested/beta_he'))
    cycle_line = b"editor_cycle.share_alpha"
    back_to_shared_prefix = b"\x1b[D" * (len(cycle_line) - len(b"editor_cycle.sh"))
    steps = [
        ("definitions", b"int editor_sum(int first, int second) { return first + second; } struct EditorInteractive { int field = 7; int method(int delta) const { return field + delta; } int zero() const { return field; } }; EditorInteractive editor_obj; EditorInteractive *editor_ptr = &editor_obj; EditorInteractive editor_make() { return {}; } int editor_call_alpha(int value) { return value + 1; } int editor_call_beta(int value) { return value + 10; }\r", True),
        ("cycle_definitions", b"struct EditorCycle { int share_alpha = 31; int share_beta = 32; }; EditorCycle editor_cycle;\r", True),
        ("function_pair", b"editor_su\t"),
        ("function_value", b"20, 22\r", True),
        ("unique_cycle_pair", b"editor_su\x0e\x0e"),
        ("unique_cycle_value", b"20, 22\r", True),
        ("method_pair", b"editor_obj.met\t"),
        ("method_value", b"5\r", True),
        ("zero_pair", b"editor_obj.ze\t"),
        ("zero_value", b"\r", True),
        ("pointer_pair", b"editor_ptr->met\t"),
        ("pointer_value", b"5\r", True),
        ("call_result_pair", b"editor_make().met\t"),
        ("call_result_value", b"5\r", True),
        ("middle_name", existing + back_to_name + b"\t"),
        ("middle_value", b"\x05\r", True),
        ("cycle_function", b"editor_call_\x0e\x0e"),
        ("cycle_value", b"5\r", True),
        ("cycle_reverse", b"editor_call_\x0e\x0e\x10"),
        ("cycle_reverse_value", b"5\r", True),
        ("cycle_restore", cycle_line + back_to_shared_prefix + b"\x0e\x0e\x0e"),
        ("cycle_restore_value", b"\x05\r", True),
        ("embedded_signature", b"%define editor_embedded cpp`editor_su\t"),
        ("cancel_embedded_signature", b"\x03", True),
        ("open_brace", b"struct EditorBrace {"),
        ("brace_definition", b"int value = 9;};\r", True),
        ("brace_value", b"EditorBrace{}.value\r", True),
        ("parenthesis_value", b"(21 * 2\r", True),
        ("char_value", b"'Z\r", True),
        ("string_size_definition", b'int editor_string_size = sizeof("a\\\"b");\r', True),
        ("string_size", b"editor_string_size\r", True),
        ("paired_backspace", b"(\x7fint editor_pair_deleted = 42;\r", True),
        ("paired_backspace_value", b"editor_pair_deleted\r", True),
        ("utf8_pair", "/* café */ (21 * 2\r".encode(), True),
        ("comment", b"// don't pair ({ \"'\r", True),
        ("quoted_include", b'#include "nested/beta_he\t\r', True),
        ("quoted_include_value", b"editor_header_value\r", True),
        ("middle_include", include_line + back_to_filename + b"\t\x05\r", True),
        ("middle_include_value", b"editor_header_value\r", True),
        ("angle_open", b"#include <"),
        ("system_include", b"system_he\t>\r", True),
        ("system_include_value", b"editor_system_value\r", True),
        ("auto_closed_include", b"#include <system_he\t\r", True),
        ("include_pair_backspace", b'#include <\x7f"nested/beta_he\t\r', True),
        ("less_than", b"21 < 42\r", True),
        ("greater_than", b"42 > 21\r", True),
        ("quit", b"%quit\r"),
    ]
    out, status = session(binary, cwd, steps, args=("-I" + str(headers),))
    check("function completion opens a call with its signature hint",
          "editor_sum(first:  (int), second:  (int)) -> int" in plain(out["function_pair"]) and
          "(int) 42" in plain(out["function_value"]))
    check("cycling after a unique completion preserves the argument cursor",
          "(int) 42" in plain(out["unique_cycle_value"]))
    check("method completion shows its signature and leaves the cursor inside",
          "method(delta:  (int))" in plain(out["method_pair"]) and
          "-> int" in plain(out["method_pair"]) and
          "(int) 12" in plain(out["method_value"]))
    check("completed zero-argument methods execute with Enter",
          "zero()" in plain(out["zero_pair"]) and
          "(int) 7" in plain(out["zero_value"]))
    for key in ("pointer_value", "call_result_value", "middle_value"):
        check(key + " preserves member access and existing call suffixes",
              "(int) 12" in plain(out[key]))
    check("cycling callable suggestions keeps parentheses and cursor consistent",
          "(int) 15" in plain(out["cycle_value"]))
    check("reverse cycling preserves parentheses and cursor position",
          "(int) 6" in plain(out["cycle_reverse_value"]))
    check("cycling back restores the remaining original identifier suffix",
          "(int) 31" in plain(out["cycle_restore_value"]))
    check("embedded C++ command bodies show function signatures",
          "editor_sum(int first, int second) -> int" in plain(out["embedded_signature"]))
    check("braces pair with the cursor inside and closers do not duplicate",
          "{}" in plain(out["open_brace"]) and "(int) 9" in plain(out["brace_value"]))
    for key in ("parenthesis_value", "utf8_pair"):
        check(key + " preserves a complete expression", "(int) 42" in plain(out[key]))
    check("Backspace removes both halves of an empty pair",
          "error:" not in plain(out["paired_backspace"]) and
          "(int) 42" in plain(out["paired_backspace_value"]))
    check("character quotes close automatically", "(char)" in plain(out["char_value"]))
    check("escaped quotes stay inside the literal", "(int) 4" in plain(out["string_size"]))
    check("comments do not acquire closing delimiters", "error:" not in plain(out["comment"]))
    check("quoted include completes nested paths without duplicating quotes",
          "(int) 73" in plain(out["quoted_include_value"]))
    check("mid-filename completion replaces the whole remaining path token",
          "error:" not in plain(out["middle_include"]) and
          "(int) 73" in plain(out["middle_include_value"]))
    check("angle include pairs and completes without duplicating its closer",
          "<>" in plain(out["angle_open"]) and
          "error:" not in plain(out["system_include"]) and
          "(int) 74" in plain(out["system_include_value"]))
    check("angle include submits with its automatic closer",
          "error:" not in plain(out["auto_closed_include"]))
    check("Backspace removes both include angle brackets",
          "error:" not in plain(out["include_pair_backspace"]))
    for key in ("less_than", "greater_than"):
        check(key + " remains an ordinary comparison", "(bool) true" in plain(out[key]))
    check("editor session exits normally", status == 0)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
