#!/usr/bin/env python3
"""Replay the editing defects found in the complete editor review."""
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
    (headers / "nested" / "review_header.hpp").write_text(
        "#pragma once\nint editor_review_header = 81;\n")
    (headers / "editor_review(1).hpp").write_text(
        "#pragma once\nint editor_review_parens = 82;\n")
    out, status = session(binary, cwd, [
        ("definitions", b"int editor_target(int value) { return value + 1; } namespace editor_ns { int editor_function(int value) { return value + 2; } } int string = 42;\r", True),
        ("address", b"auto editor_pointer = &editor_tar\t;\r", True),
        ("address_value", b"editor_pointer(41)\r", True),
        ("typed_address", b"int (*editor_typed_pointer)(int) = editor_tar\t;\r", True),
        ("typed_address_value", b"editor_typed_pointer(41)\r", True),
        ("grouped_address", b"auto editor_grouped_pointer = &(editor_tar\t);\r", True),
        ("grouped_address_value", b"editor_grouped_pointer(41)\r", True),
        ("using", b"using editor_ns::editor_fun\t;\r", True),
        ("using_value", b"editor_function(40)\r", True),
        ("group_open", b"21 * ( \r"),
        ("group_value", b"2\r", True),
        ("macro_open", b'#define EDITOR_REVIEW_MESSAGE "abc\\\r'),
        ("macro_middle", b"def\\\r"),
        ("macro_close", b"ghi\r", True),
        ("macro_value", b"sizeof(EDITOR_REVIEW_MESSAGE) == 10\r", True),
        ("comment_include", b'#include /* comment */ "nested/review_he\t\r', True),
        ("comment_include_value", b"editor_review_header\r", True),
        ("splice_open", b"#include \\\r"),
        ("splice_include", b'"nested/review_he\t\r', True),
        ("header_splice_open", b'#include "nested/review_\\\r'),
        ("header_splice_complete", b"he\t\r", True),
        ("angle_filename", b"#include <editor_review(1).hpp>\r", True),
        ("angle_value", b"editor_review_parens\r", True),
        ("type_named_variable", b"string + 1"),
        ("type_named_value", b"\r", True),
        ("quit", b"%quit\r"),
    ], args=("-I" + str(headers),))
    for name in ("address", "typed_address", "grouped_address", "using", "group_open",
                 "macro_open", "macro_middle", "macro_close", "comment_include",
                 "splice_include", "header_splice_open", "header_splice_complete", "angle_filename"):
        text = plain(out[name])
        check(name + " remains valid editor input", "error:" not in text and "warning:" not in text)
    for name in ("address_value", "typed_address_value", "grouped_address_value", "using_value", "group_value"):
        check(name + " executes the intended expression", "(int) 42" in plain(out[name]))
    check("macro continuation preserves the paired quote", "(bool) true" in plain(out["macro_value"]))
    check("comment-separated include completes nested paths", "(int) 81" in plain(out["comment_include_value"]))
    check("angle filename punctuation stays literal", "(int) 82" in plain(out["angle_value"]))
    check("lexical type names receive semantic variable colors",
          b"\x1b[0;22;37mstring" in out["type_named_variable"] and
          "(int) 43" in plain(out["type_named_value"]))
    check("review regression session exits normally", status == 0)
    if helpers["failures"]:
        for name, output in out.items():
            print(name + ": " + plain(output))
sys.exit(bool(helpers["failures"]))
