#!/usr/bin/env python3
"""Terminal regression for rich Clang completion, signatures and semantic colors."""
import os
from pathlib import Path
import runpy
import sys
import tempfile

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])
struct_line = b"struct C { int x = 1; int y = 0;  };"
initializer_cursor = b"\x1b[D" * (len(struct_line) - struct_line.index(b"=") - 1)
with tempfile.TemporaryDirectory() as cwd:
    out, status = session(
        binary, cwd,
        [
            ("definitions", b"int editor_menu(int first) { return first; } double editor_menu(double value) { return value; } struct EditorPty { int field = 7; int method() const { return field; } }; EditorPty editor_pty;\r", True),
            ("rich_menu", b"editor_\t"),
            ("cancel_rich_menu", b"\x03", True),
            ("signature", b"editor_men\t\t"),
            ("cancel_signature", b"\x03", True),
            ("rich_call", b"editor_menu(2)\r", True),
            ("rich_members", b"editor_pty.\t"),
            ("cancel_members", b"\x03", True),
            ("semantic_field", b"editor_pty.field"),
            ("field_value", b"\r", True),
            ("hint_select", b"editor_menu(\x1b[1;5B\t"),
            ("cancel_hint_menu", b"\x03\x03", True),
            ("after_hint", b"21 * 2\r", True),
            ("directive_menu", b"#\t\t"),
            ("cancel_directive_menu", b"\x03", True),
            ("directive_prefix", b"#def\t"),
            ("define_completed", b" EDITOR_PTY_DIRECTIVE 11\r", True),
            ("directive_value", b"EDITOR_PTY_DIRECTIVE\r", True),
            ("include_prefix", b"#i\t\t"),
            ("cancel_include", b"\x03", True),
            ("middle_initializer", struct_line + initializer_cursor + b"\t"),
            ("cancel_middle_initializer", b"\x03\x03", True),
            ("after_middle_initializer", b"21 * 2\r", True),
            ("quit", b"%quit\r"),
        ],
    )
    menu = plain(out["rich_menu"])
    check("overload menu shows parameters and return types",
          "editor_menu(int first) -> int" in menu and
          "editor_menu(double value) -> double" in menu)
    check("signature hints show inline argument names and types",
          ":  (" in plain(out["signature"]) and "->" in plain(out["signature"]))
    check("completion labels are not inserted into C++", "(int) 2" in plain(out["rich_call"]))
    check("member completion menu distinguishes fields and methods",
          "field -> int [field]" in plain(out["rich_members"]) and
          "method() const -> int [method]" in plain(out["rich_members"]))
    check("semantic field color reaches the terminal", b"\x1b[0;22;36mfield" in out["semantic_field"])
    check("semantic editing preserves execution", "(int) 7" in plain(out["field_value"]))
    check("hint cycling and completion leave the REPL usable",
          "(int) 42" in plain(out["after_hint"]) and status == 0)
    directives = plain(out["directive_menu"])
    check("hash offers directives instead of C++ symbols",
          "include [keyword]" in directives and "define [keyword]" in directives and
          "EditorPty" not in directives and "editor_menu" not in directives)
    check("a unique directive prefix inserts only its keyword",
          "(int) 11" in plain(out["directive_value"]))
    check("hash-i filters directive choices by prefix",
          "include [keyword]" in plain(out["include_prefix"]) and
          "define [keyword]" not in plain(out["include_prefix"]))
    check("Tab inside a field initializer leaves the editor responsive",
          "(int) 42" in plain(out["after_middle_initializer"]) and status == 0)
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")
sys.exit(bool(helpers["failures"]))
