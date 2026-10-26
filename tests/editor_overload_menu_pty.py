#!/usr/bin/env python3
"""Exercise overload selection and editor navigation through a real terminal."""
import os
import pty
from pathlib import Path
import runpy
import select
import signal
import sys
import tempfile
import time
import re

helpers = runpy.run_path(str(Path(__file__).with_name("pty_smoke.py")))
session, plain, check = (helpers[key] for key in ("session", "plain", "check"))
binary = os.path.abspath(sys.argv[1])


def fast_menu_probe(binary):
    with tempfile.TemporaryDirectory() as cwd:
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(cwd)
            os.environ["HOME"] = cwd
            os.environ["TERM"] = "xterm-256color"
            os.execv(binary, [binary])

        def read_for(duration):
            output = b""
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], deadline - time.monotonic())
                if not ready:
                    break
                try:
                    output += os.read(fd, 65536)
                except OSError:
                    break
            return output.decode("utf8", "replace")

        try:
            read_for(0.3)
            os.write(fd, b"int quick_menu(int a); int quick_menu(int a,int b);\r")
            read_for(2.0)
            os.write(fd, b"quick_menu(")
            read_for(0.05)
            os.write(fd, b"\t")
            opened = read_for(0.05)
            os.write(fd, b"\x1b[B")
            moved = read_for(0.05)
            strip = lambda value: re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", value)
            opened, moved = strip(opened), strip(moved)
            return ("quick_menu(int a)" in opened and "quick_menu(int a, int b)" in opened and
                    moved.rfind("> quick_menu(int a, int b)") > moved.rfind("> quick_menu(int a)"))
        finally:
            try:
                os.write(fd, b"\x03%quit\r")
            except OSError:
                pass
            deadline = time.monotonic() + 1.0
            exited = False
            while time.monotonic() < deadline:
                done, _ = os.waitpid(pid, os.WNOHANG)
                if done:
                    exited = True
                    break
                time.sleep(0.02)
            if not exited:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, os.WNOHANG)

with tempfile.TemporaryDirectory() as cwd:
    out, status = session(binary, cwd, [
        ("definitions", b"int menu_effects = 0; int menu_pick(int first) { ++menu_effects; return 100 + first; } int menu_pick(int first, int second) { ++menu_effects; return 200 + first + second; } struct MenuObject { int choose(int value) { return 300 + value; } int choose(int left, int right) { return 400 + left + right; } }; MenuObject menu_object;\r", True),
        ("call_open", b"menu_pick("),
        ("menu_open", b"\t"),
        ("menu_down", b"\x1b[B"),
        ("menu_accept", b"\t"),
        ("first_arg", b"\t20"),
        ("second_arg", b"\x1b[Z10"),
        ("close_and_run", b"\x1b[C\x1b[C\x1b[C\x1b[C\r", True),
        ("selected_effect", b"menu_effects\r", True),
        ("method_open", b"menu_object.choose("),
        ("method_menu", b"\t"),
        ("method_accept", b"\t"),
        ("method_first", b"3,"),
        ("method_second", b" 4"),
        ("method_close_run", b"\x1b[C\r", True),
        ("cancel_open", b"menu_pick("),
        ("cancel_menu", b"\t"),
        ("cancel", b"\x03", True),
        ("cancel_run", b"menu_pick(12)\r", True),
        ("edit_open", b"menu_pick("),
        ("edit_menu", b"\t"),
        ("edit_dismiss", b"12"),
        ("edit_close", b"\x1b[C\r", True),
        ("cursor_open", b"menu_pick("),
        ("cursor_menu", b"\t"),
        ("cursor_move", b"\x01"),
        ("cursor_tab", b"\t"),
        ("cursor_cancel", b"\x03", True),
        ("prefix", b"menu_pi\t"),
        ("prefix_down", b"\x1b[B"),
        ("prefix_accept", b"\t"),
        ("prefix_args", b"7, 8"),
        ("prefix_finish", b"\x1b[C\r", True),
        ("enter_open", b"menu_pick("),
        ("enter_menu", b"\t"),
        ("enter_accept", b"\r"),
        ("enter_args", b"9, 10)\r", True),
        ("zero_defs", b"int zero_effects=0; int zero_menu() { return ++zero_effects; } int zero_menu(int value) { ++zero_effects; return value; }\r", True),
        ("zero_open", b"zero_menu("),
        ("zero_menu_list", b"\t"),
        ("zero_stale_cursor", b"\x01"),
        ("zero_stale_enter", b"\r"),
        ("zero_cancel", b"\x03", True),
        ("zero_effect_count", b"zero_effects\r", True),
        ("rapid_defs", b"int rapid_effects=0; int rapid_zero() { return ++rapid_effects; } int rapid_zero(int value) { ++rapid_effects; return value; }\r", True),
        ("rapid_open", b"rapid_zero("),
        ("rapid_menu", b"\t"),
        ("rapid_edit_enter", b"\x1b[D \r", True),
        ("rapid_effect_count", b"rapid_effects\r", True),
        ("template_defs", b"template<class T> int menu_template(T value) { return 500; } template<class T> int menu_template(T first, T second) { return 600 + first + second; }\r", True),
        ("template_open", b"menu_template<int>("),
        ("template_menu", b"\t"),
        ("template_down", b"\x1b[B"),
        ("template_accept", b"\t"),
        ("template_args", b"7, 8)\r", True),
        ("pair_open", b"menu_pick (\x1b[D\x1b[D"),
        ("pair_menu", b"\t"),
        ("pair_down", b"\x1b[B"),
        ("pair_accept", b"\t"),
        ("pair_args", b"\x1b[C\x1b[C5, 6"),
        ("pair_finish", b"\x1b[C\r", True),
        ("distinct_defs", b"int menu_foo(int value); int menu_food(int value); int menu_value;\r", True),
        ("distinct_prefix", b"menu_f\t"),
        ("distinct_cancel", b"\x03", True),
        ("many_defs", b"int many_pick_with_a_really_long_name(int parameter_with_a_very_long_name); int many_pick_with_a_really_long_name(int parameter_with_a_very_long_name,int another_very_long_parameter_name); int many_pick_with_a_really_long_name(int a,int b,int c); int many_pick_with_a_really_long_name(int a,int b,int c,int d); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e,int f); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e,int f,int g); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e,int f,int g,int h); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e,int f,int g,int h,int i); int many_pick_with_a_really_long_name(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j);\r", True),
        ("many_open", b"many_pick_with_a_really_long_name("),
        ("many_menu", b"\t"),
        ("many_cancel", b"\x03", True),
        ("history", b"\x1b[A\x1b[B"),
        ("quit", b"%quit\r"),
    ])

    opened = plain(out["menu_open"])
    check("Tab opens a menu containing every matching global signature",
          "menu_pick(int first)" in opened and
          "menu_pick(int first, int second)" in opened)
    check("menu navigation shows the selected overload parameters",
          "first:" in plain(out["menu_accept"]) and
          "second:" in plain(out["menu_accept"]))
    down_menu = plain(out["menu_down"])
    check("Down visibly moves the selected row to the next signature",
          down_menu.rfind("> menu_pick(int first, int second)") >
          down_menu.rfind("> menu_pick(int first)"))
    check("selected overload continues to drive hints while arguments are typed",
          "second:" in plain(out["first_arg"]))
    check("selected overload leaves C++ resolution to argument types",
          "(int) 230" in plain(out["close_and_run"]) and
          "(int) 1" in plain(out["selected_effect"]))
    check("method overloads are listed and the selected method drives hints",
          "choose(int value)" in plain(out["method_menu"]) and
          "choose(int left, int right)" in plain(out["method_menu"]) and
          "left:" in plain(out["method_accept"]) and
          "right:" in plain(out["method_accept"]))
    check("selected method runs using ordinary C++ overload resolution",
          "(int) 407" in plain(out["method_close_run"]))
    check("Ctrl+C cancels menu browsing without executing partial source",
          "^C" in plain(out["cancel"]) and "error:" not in plain(out["cancel_run"]) and
          "(int) 112" in plain(out["cancel_run"]))
    check("typing dismisses the menu and remains in the source buffer",
          "error:" not in plain(out["edit_close"]) and "(int) 112" in plain(out["edit_close"]))
    check("moving the cursor invalidates a stale menu before a fast Tab",
          "menu_pick(int first, int second)" not in plain(out["cursor_tab"]))
    check("completion prefixes can open and navigate the overload menu",
          "menu_pick(int first, int second)" in plain(out["prefix"]) and
          "> menu_pick(int first, int second)" in plain(out["prefix_down"]))
    check("Tab accepts a prefix overload and creates the selected call context",
          "second:" in plain(out["prefix_accept"]) and "(int) 215" in plain(out["prefix_finish"]))
    check("overload acceptance reuses an existing call after the name and whitespace",
          "menu_pick() ()" not in plain(out["pair_accept"]) and
          "second:" in plain(out["pair_args"]) and "(int) 211" in plain(out["pair_finish"]))
    check("Enter accepts the highlighted overload without submitting the line",
          "error:" not in plain(out["enter_accept"]) and "(int) 219" in plain(out["enter_args"]))
    check("a stale menu cannot execute a zero-argument overload on Enter",
          "^C" in plain(out["zero_cancel"]) and "(int) 0" in plain(out["zero_effect_count"]))
    check("an edit after cursor dismissal clears stale-menu Enter suppression immediately",
          "(int) 1" in plain(out["rapid_edit_enter"]) and
          "(int) 1" in plain(out["rapid_effect_count"]))
    check("explicit template calls open the semantic overload family and accept a selection",
          "menu_template" in plain(out["template_menu"]) and
          "second:" in plain(out["template_accept"]) and
          "(int) 615" in plain(out["template_args"]))
    check("completion prefixes with different callable names do not open an overload picker",
          "> menu_foo(int value)" not in plain(out["distinct_prefix"]) and
          "> menu_food(int value)" not in plain(out["distinct_prefix"]))
    menu_rows = [line for line in plain(out["many_menu"]).splitlines()
                 if "many_pick_with_a_really_long_name(" in line and (line.startswith("> ") or line.startswith("  "))]
    check("large overload sets keep the selected row visible in a bounded menu",
          len({line[2:] for line in menu_rows}) <= 8 and
          any(line.startswith("> ") for line in menu_rows))
    check("long menu rows are clipped with an ellipsis",
          "…" in plain(out["many_menu"]))
    saved = (Path(cwd) / ".replxx_history").read_text()
    check("menus and parameter annotations never enter history",
          "menu_pick(first:" not in saved and "-> int" not in saved and
          "menu_object.choose(left:" not in saved)
    check("history Up and Down still work outside a menu", "error:" not in plain(out["history"]))
    check("overload menu session exits normally", status == 0)
    check("menu opening and selection redraw within 50 ms", fast_menu_probe(binary))
    if helpers["failures"]:
        for key, value in out.items():
            print(f"{key}: {plain(value)}")

sys.exit(bool(helpers["failures"]))
