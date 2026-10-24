#!/usr/bin/env python3
"""Drives cpp-repl through a pseudo-terminal.

Usage: pty_smoke.py <path-to-cpp-repl>
Runs in a scratch directory so the real history file is never touched.
"""
import os
import pty
import re
import select
import signal
import subprocess
import sys
import tempfile
import time


def session(binary, cwd, steps):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        # This test needs replxx's full terminal mode even when CTest's parent
        # shell uses TERM=dumb (as many automation runners do).
        os.environ["TERM"] = "xterm-256color"
        os.execv(binary, [binary])

    # Reads until the output goes quiet; with `wait`, first until the next
    # prompt appears (compiling and running C++ takes a moment).
    def read(wait=False, timeout=30.0):
        out = b""
        deadline = time.time() + timeout
        if not wait:
            time.sleep(0.4)
        while time.time() < deadline:
            if not select.select([fd], [], [], 0.3)[0]:
                if not wait or PROMPT in out:
                    break
                continue
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            out += data
        return out

    outputs = {"start": read(wait=True)}
    for name, keys, *wait in steps:
        os.write(fd, keys)
        outputs[name] = read(wait=bool(wait and wait[0]))
    time.sleep(0.5)
    done, status = os.waitpid(pid, os.WNOHANG)
    if done == 0:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        status = -1
    return outputs, status


PROMPT = b"cpp-repl\x1b[0m> "


def plain(raw):
    return re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", raw).decode("utf8", "replace")


failures = []


def check(label, ok):
    print(("PASS " if ok else "FAIL ") + label)
    if not ok:
        failures.append(label)


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as cwd:
        out, status = session(
            binary,
            cwd,
            [
                ("open", b"int twice(int x) {"),
                ("enter1", b"\r"),
                ("blank", b"\r"),
                ("spaces", b"    \r"),
                ("close", b"return x * 2; }"),
                ("enter2", b"\r", True),
                ("call", b"twice(21)\r", True),
                ("class_open", b"class BlankPoint { int inc(int n) { return n+1; }\r"),
                ("class_blank", b"\r"),
                ("class_close", b"public: int run_inc(int n) { return inc(n); } };\r", True),
                ("class_call", b"BlankPoint{}.run_inc(3)\r", True),
                ("ctrl_c_text", b"g("),
                ("enter4", b"\r"),
                ("ctrl_c", b"\x03", True),
                ("quit", b"%quit\r"),
            ],
        )
        check("Enter after 'int twice(int x) {' does not submit", "error" not in plain(out["enter1"]))
        check("Enter after 'return x * 2; }' compiles the block", "error" not in plain(out["enter2"]))
        check("the defined function runs in the JIT", "(int) 42" in plain(out["call"]))
        check("blank and whitespace lines continue an incomplete block", all(
            b"^C" not in out[key] and "error" not in plain(out[key])
            for key in ("blank", "spaces")))
        check("a class with a blank line before public compiles and runs",
            b"^C" not in out["class_blank"] and
            "error" not in plain(out["class_close"]) and "(int) 4" in plain(out["class_call"]))
        check("Ctrl+C cancels an incomplete block", b"^C" in out["ctrl_c"] and "error" not in plain(out["ctrl_c"]))
        check("%quit exits with status 0", status == 0)
        with open(os.path.join(cwd, ".replxx_history"), encoding="utf8") as f:
            check("multi-line entry preserves blank lines in history", "int twice(int x) {\x17\x17    \x17return x * 2; }" in f.read())  # replxx stores newlines as 0x17

        out, status = session(
            binary,
            cwd,
            [
                ("code", b'return "s\\n" // c'),
                ("enter", b"\r", True),
                ("cmd", b"%help"),
                ("enter2", b"\r", True),
                ("bad", b"%nope"),
                ("enter3", b"\r", True),
                ("pathlist", b"%add-path /usr/lib:/tmp"),
                ("enter4", b"\r", True),
                ("trailing", b"%add-path /tmp:"),
                ("enter5", b"\r", True),
                ("extra", b"%help me"),
                ("enter6", b"\r", True),
                ("ctrl_d", b"\x04"),
            ],
        )
        code = out["code"]
        check("keyword is magenta", b"\x1b[0;22;35mreturn" in code)
        check("string is green", b'\x1b[0;22;32m"s' in code)
        check("escape is yellow", b"\x1b[0;93m\\n" in code)
        check("comment is gray", b"\x1b[0;90m// c" in code)
        check("%command is bright green", re.search(rb"\x1b\[[0-9;]*92m%help", out["cmd"]) is not None)
        check("unknown %command is bright red", re.search(rb"\x1b\[[0-9;]*91m%nope", out["bad"]) is not None)
        check("path operands are bright blue", re.search(rb"\x1b\[[0-9;]*94m/usr/lib", out["pathlist"]) is not None)
        check("':' separator is yellow", re.search(rb"\x1b\[[0-9;]*[39]3m:", out["pathlist"]) is not None)
        check("dangling ':' is bright red", re.search(rb"\x1b\[[0-9;]*91m:", out["trailing"]) is not None)
        check("dangling ':' is rejected with usage", "usage: %add-path" in plain(out["enter5"]))
        check("argument to %help is bright red", re.search(rb"\x1b\[[0-9;]*91mme", out["extra"]) is not None)
        check("Ctrl+D exits with status 0", status == 0)

        out, status = session(
            binary,
            cwd,
            [
                ("help", b"%help\r", True),
                ("bad_path", b"%add-path /no/such/dir\r", True),
                ("good_path", b"%add-path /usr/lib\r", True),
                ("path_list", b"%add-path /tmp:/no/such:/\r", True),
                ("lib_ok", b"%lib z\r", True),
                ("zlib_decl", b'extern "C" const char* zlibVersion();\r', True),
                ("zlib_call", b"zlibVersion()\r", True),
                ("lib_missing", b"%lib no_such_library_xyz\r", True),
                ("lib_quote", b"%lib don't\r", True),
                ("unknown", b"%exit\r", True),
                ("plain_exit", b"exit\r", True),
                ("include", b"#include <iostream>\r", True),
                ("cout", b'std::cout << "hi from the JIT" << std::endl;\r', True),
                ("undefined", b"int nope(); nope()\r", True),
                ("after_error", b"1 + 2\r", True),
                ("quit", b"%quit\r"),
            ],
        )
        help_text = plain(out["help"])
        check("%help lists the sixteen commands", all(c in help_text for c in ("%lib", "%libs", "%libinfo", "%syminfo", "%add-path", "%add", "%class", "%info", "%symbols", "%mock", "%pre", "%post", "%restore", "%hooks", "%help", "%quit")))
        check("%add-path rejects a missing dir", "error:" in plain(out["bad_path"]))
        check("%add-path accepts an existing dir", "error:" not in plain(out["good_path"]))
        path_list = plain(out["path_list"])
        check("%add-path adds each dir in a ':' list", path_list.count("added:") == 2 and "no such directory: /no/such" in path_list)
        check("%lib loads libz", "error:" not in plain(out["lib_ok"]) and "libz" in plain(out["lib_ok"]))
        check("JIT-ed code calls into the %lib library", '(const char *) "1.' in plain(out["zlib_call"]))
        check("%lib reports a missing library", "error:" in plain(out["lib_missing"]))
        check("%lib with a quote submits instead of continuing", "error:" in plain(out["lib_quote"]))
        check("unknown %exit suggests %help", "try %help" in plain(out["unknown"]))
        check("plain 'exit' is C++ input (a compile error)", "error" in plain(out["plain_exit"]))
        check("std::cout output from the JIT", "hi from the JIT" in plain(out["cout"]))
        check("an undefined symbol is reported", "undefined symbol: nope()" in plain(out["undefined"]))
        check("the REPL keeps working after an error", "(int) 3" in plain(out["after_error"]))
        check("%quit exits with status 0 before EOF", status == 0)

        out, status = session(
            binary,
            cwd,
            [
                ("decl", b"int lab_total(int a, int b) { return a + b; }\r", True),
                ("struct", b"struct Point { int x, y; int sum() const { return x + y; } };\r", True),
                ("object", b"Point pt{1, 2};\r", True),
                ("tab_name", b"lab_t\t"),
                ("call", b"(1, 2)\r", True),
                ("tab_member", b"pt.\t"),
                ("cancel", b"\x03", True),
                ("class_list", b"%class\r", True),
                ("class_tab", b"%class Po\t"),
                ("class_show", b"\r", True),
                ("quit", b"%quit\r"),
            ],
        )
        check("Tab completes a session function", "lab_total" in plain(out["tab_name"]))
        check("the completed call runs", "(int) 3" in plain(out["call"]))
        members = plain(out["tab_member"])
        check("Tab after 'pt.' lists methods and fields", all(m in members for m in ("sum", "x", "y")))
        check("%class lists user classes", "Point" in plain(out["class_list"]))
        check("Tab completes a class name after %class", "Point" in plain(out["class_tab"]))
        shown = plain(out["class_show"])
        check("%class Point shows the layout", "struct Point {  // size 8, align 4" in shown and "int x;" in shown)
        check("%class Point shows the methods", "int sum() const;" in shown)
        check("%quit exits with status 0 after completion", status == 0)

        with open(os.path.join(cwd, "geometry.cpp"), "w", encoding="utf8") as f:
            f.write("struct Circle { double r; double area() const { return 3 * r * r; } };\n"
                    "double circle_area(double r) { return Circle{r}.area(); }\n")
        out, status = session(
            binary,
            cwd,
            [
                ("tab_file", b"%add geo\t"),
                ("add", b"\r", True),
                ("call", b"circle_area(2.0)\r", True),
                ("classes", b"%class\r", True),
                ("again", b"%add geometry.cpp\r", True),
                ("missing", b"%add nope.cpp\r", True),
                ("list", b"%add\r", True),
                ("quit", b"%quit\r"),
            ],
        )
        check("Tab completes a file name after %add", "geometry.cpp" in plain(out["tab_file"]))
        check("%add parses the file", "added: " in plain(out["add"]) and "geometry.cpp" in plain(out["add"]))
        check("a function from the added file runs", "(double) 12.0" in plain(out["call"]))
        check("%class lists a class from the added file", "Circle" in plain(out["classes"]))
        check("a file is added only once", "already added" in plain(out["again"]))
        check("%add reports a missing file", "no such file: nope.cpp" in plain(out["missing"]))
        check("%add lists the added files", "geometry.cpp" in plain(out["list"]))
        check("%quit exits with status 0 after %add", status == 0)

        out, status = session(
            binary,
            cwd,
            [
                ("define", b"int terminal_hook(int x) { return x*2; }\r", True),
                ("hooks_empty", b"%hooks\r", True),
                ("open", b"%mock int terminal_hook(int x) cpp`"),
                ("newline", b"\r"),
                ("hook_blank", b"\r"),
                ("body", b"return x+10;\r"),
                ("close", b"`\r", True),
                ("pre", b"%pre terminal_hook(int x) cpp`x+=1;`\r", True),
                ("post", b"%post terminal_hook(int x) cpp`result*=2;`\r", True),
                ("hooks_all", b"%hooks\r", True),
                ("hooks_extra", b"%hooks extra\r", True),
                ("call", b"terminal_hook(3)\r", True),
                ("clear", b"%mock terminal_hook(int x) cpp``\r", True),
                ("hooks_cleared", b"%hooks\r", True),
                ("original", b"terminal_hook(3)\r", True),
                ("invalid", b"%mock terminal_hook(int x) cpp`return missing_name;`\r", True),
                ("recover", b"terminal_hook(3)\r", True),
                ("remock", b"%mock terminal_hook(int x) cpp`return x+10;`\r", True),
                ("restore", b"%restore terminal_hook(int x)\r", True),
                ("hooks_restored", b"%hooks\r", True),
                ("restored_call", b"terminal_hook(3)\r", True),
                ("restore_again", b"%restore terminal_hook(int x)\r", True),
                ("global_pre", b"%pre terminal_hook(int x) cpp`x+=2;`\r", True),
                ("global_mock", b"%mock terminal_hook(int x) cpp`return 99;`\r", True),
                ("global_post", b"%post terminal_hook(int x) cpp`result*=2;`\r", True),
                ("restore_all", b"%restore\r", True),
                ("global_original", b"terminal_hook(3)\r", True),
                ("restore_empty", b"%restore  \r", True),
                ("cancel_open", b"%pre terminal_hook(int x) cpp`\r"),
                ("cancel_blank", b"\r"),
                ("cancel_hook", b"\x03", True),
                ("after_cancel", b"terminal_hook(3)\r", True),
                ("quit", b"%quit\r"),
            ],
        )
        check("open cpp block continues on Enter", "error" not in plain(out["newline"]))
        check("blank line continues an incomplete hook", b"^C" not in out["hook_blank"])
        check("multiline mock compiles on closing backtick", "%mock set:" in plain(out["close"]))
        check("pre, mock and post affect the call", "(int) 28" in plain(out["call"]))
        check("hooks reports an empty session", "no active mocks or hooks" in plain(out["hooks_empty"]))
        listing = plain(out["hooks_all"]).replace("\r\n", "\n")
        check("hooks lists signatures and complete C++ bodies", all(
            entry in listing for entry in ("%mock int terminal_hook(int x) cpp`\n\nreturn x+10;\n`",
                "%pre terminal_hook(int x) cpp`x+=1;`",
                "%post terminal_hook(int x) cpp`result*=2;`")))
        check("hooks rejects arguments", "%hooks takes no arguments" in plain(out["hooks_extra"]))
        listing = plain(out["hooks_cleared"])
        check("hooks omits a cleared mock and its body", "%mock " not in listing and
            "return x+10;" not in listing and "%pre terminal_hook" in listing)
        check("hooks omits restored entries", "no active mocks or hooks" in plain(out["hooks_restored"]))
        check("empty mock restores original with hooks", "(int) 16" in plain(out["original"]))
        check("invalid mock is rejected", "error" in plain(out["invalid"]))
        check("hooks survive a rejected mock", "(int) 16" in plain(out["recover"]))
        check("restore reports success", "restored: terminal_hook(int x)" in plain(out["restore"]))
        check("restore removes mock and both hooks", "(int) 6" in plain(out["restored_call"]))
        check("repeated restore succeeds", "restored:" in plain(out["restore_again"]))
        check("bare restore clears the session", "restored all mocks and hooks" in plain(out["restore_all"]))
        check("bare restore returns calls to the original", "(int) 6" in plain(out["global_original"]))
        check("bare restore on an empty session succeeds", "restored all mocks and hooks" in plain(out["restore_empty"]))
        check("Ctrl+C cancels an incomplete hook after a blank line",
            b"^C" not in out["cancel_blank"] and b"^C" in out["cancel_hook"] and
            "(int) 6" in plain(out["after_cancel"]))
        check("%quit exits after hooks", status == 0)

        out, status = session(binary, cwd, [
            ("define", b"int symbol_total(int a, int b) { return a+b; }\r", True),
            ("tab", b"%info symbol_t\t"),
            ("info", b"\r", True),
            ("missing", b"%info no_such_symbol\r", True),
            ("no_argument", b"%info\r", True),
            ("extra_argument", b"%info symbol_total extra\r", True),
            ("symbols_tab_lib", b"%symbols l\t"),
            ("symbols_cancel_lib", b"\x03", True),
            ("symbols_tab_name", b"%symbols lib '<session>' n\t"),
            ("symbols_cancel_name", b"\x03", True),
            ("symbols_tab_all", b"%symbols a\t"),
            ("symbols_cancel_all", b"\x03", True),
            ("symbols_tab_after_all", b"%symbols all n\t"),
            ("symbols_cancel_after_all", b"\x03", True),
            ("symbols", b'%symbols lib "<session>" name "^symbol_total\\(int, int\\)$"\r', True),
            ("symbols_system_hidden", b"%symbols name '^malloc$'\r", True),
            ("symbols_system_all", b"%symbols all name '^malloc$'\r", True),
            ("symbols_missing", b"%symbols lib no_such_image_987654321\r", True),
            ("symbols_invalid", b'%symbols name "["\r', True),
            ("symbols_usage", b"%symbols unknown\r", True),
            ("libs_tab_all", b"%libs a\t"),
            ("libs_cancel_all", b"\x03", True),
            ("libraries_hidden", b"%libs '^libz[.]'\r", True),
            ("libraries", b"%libs all '^libz[.]'\r", True),
            ("libraries_all", b"%libs all\r", True),
            ("libraries_invalid", b"%libs all '['\r", True),
            ("library_info", b"%libinfo '^libz[.]'\r", True),
            ("symbol_info", b"%syminfo 'symbol_total'\r", True),
            ("still_runs", b"symbol_total(2, 3)\r", True),
            ("quit", b"%quit\r"),
        ])
        check("Tab completes names after info", "symbol_total" in plain(out["tab"]))
        info = plain(out["info"])
        check("info shows declaration and JIT address", "Symbol: symbol_total" in info and
            "Declaration : int symbol_total(int a, int b)" in info and "Address     : 0x" in info)
        fields = [line for line in info.splitlines() if " : " in line and line.startswith("  ")]
        check("info metadata columns align", bool(fields) and all(line.find(" : ") == 13 for line in fields))
        check("info reports unknown symbols", "no symbol named 'no_such_symbol'" in plain(out["missing"]))
        check("info requires a symbol", "usage: %info <symbol>" in plain(out["no_argument"]))
        check("info rejects extra arguments", "%info takes one symbol name" in plain(out["extra_argument"]))
        check("info leaves the session usable", "(int) 5" in plain(out["still_runs"]) and status == 0)
        symbols = plain(out["symbols"])
        check("symbols completes the lib keyword", "%symbols lib" in plain(out["symbols_tab_lib"]))
        check("symbols completes the name keyword after a library filter",
            "name" in plain(out["symbols_tab_name"]))
        check("symbols completes all and keywords after all",
            "%symbols all" in plain(out["symbols_tab_all"]) and
            "%symbols all name" in plain(out["symbols_tab_after_all"]))
        check("symbols hides system exports unless all is requested",
            "\nmalloc\n" not in plain(out["symbols_system_hidden"]).replace("\r", "") and
            "\nmalloc\n" in plain(out["symbols_system_all"]).replace("\r", ""))
        check("symbols lists qualified signatures without columns or counts",
            "symbol_total(int, int)" in symbols and all(text not in symbols for text in
                ("LIBRARY", "KIND", "SYMBOL", "1 symbol")))
        check("symbols returns an empty list without a diagnostic", "error:" not in plain(out["symbols_missing"]) and
            "no loaded symbols match" not in plain(out["symbols_missing"]))
        check("symbols reports an invalid regex", "invalid name regex" in plain(out["symbols_invalid"]))
        check("symbols rejects unknown options with usage", "usage: %symbols" in plain(out["symbols_usage"]))
        check("libs lists names without paths", "libz." in plain(out["libraries"]) and "/usr/" not in plain(out["libraries"]))
        check("libs completes all", "%libs all" in plain(out["libs_tab_all"]))
        check("libs hides system libraries unless all is requested",
            "libz." not in plain(out["libraries_hidden"]) and
            "libz." in plain(out["libraries_all"]))
        check("libs all validates its regex",
            "invalid library regex" in plain(out["libraries_invalid"]))
        details = plain(out["library_info"])
        check("libinfo shows a path and version", all(text in details for text in ("Library:", "Path", "Version")))
        check("syminfo shows definition and resolution state", all(text in plain(out["symbol_info"]) for text in ("Defined", "Resolved", "Source refs")))
        if "NO_COLOR" not in os.environ:
            check("symbol names are colored in a terminal", b"\x1b[36msymbol_total(int, int)\x1b[0m" in out["symbols"])
            check("information labels are colored in a terminal", b"\x1b[36m  Kind" in out["symbol_info"])

        out, status = session(binary, cwd, [
            ("define", b"int caller_add(int x) { return x+1; } struct Callers { int inc(int n) { return caller_add(n); } int double_it(int n) { return caller_add(n)*2; } }; Callers callers;\r", True),
            ("warm", b"callers.inc(3)\r", True),
            ("mock", b"%mock caller_add(int x) in Callers::inc(int n) cpp`return 99;`\r", True),
            ("inc", b"callers.inc(3)\r", True),
            ("double", b"callers.double_it(3)\r", True),
            ("direct", b"caller_add(3)\r", True),
            ("list", b"%hooks\r", True),
            ("restore_pair", b"%restore caller_add(int x) in Callers::inc(int n)\r", True),
            ("pair_original", b"callers.inc(3)\r", True),
            ("mock_again", b"%mock caller_add(int x) in Callers::inc(int n) cpp`return 98;`\r", True),
            ("restore_target", b"%restore caller_add(int x)\r", True),
            ("target_original", b"callers.inc(3)\r", True),
            ("mock_all", b"%mock caller_add(int x) in Callers::inc(int n) cpp`return 97;`\r", True),
            ("restore_all", b"%restore\r", True),
            ("all_original", b"callers.inc(3)\r", True),
            ("empty", b"%hooks\r", True),
            ("quit", b"%quit\r"),
        ])
        check("caller-scoped mock installs", "%mock set:" in plain(out["mock"]))
        check("caller-scoped mock affects inc", "(int) 99" in plain(out["inc"]))
        check("other caller keeps the original", "(int) 8" in plain(out["double"]))
        check("direct calls keep the original", "(int) 4" in plain(out["direct"]))
        check("hooks shows caller scope and code", "%mock caller_add(int x) in Callers::inc(int n) cpp`return 99;`" in plain(out["list"]))
        for key in ("pair_original", "target_original", "all_original"):
            check(key + " removes the scoped mock", "(int) 4" in plain(out[key]))
        check("restored scoped entries disappear", "no active mocks or hooks" in plain(out["empty"]))
        check("%quit exits after caller-scoped mocks", status == 0)

        pipe = subprocess.run([binary], cwd=cwd, text=True, capture_output=True,
            input="int pipe_hook(int x) { return x; }\n"
                  "%mock pipe_hook(int x) cpp`\nreturn x+7;\n`\n"
                  "%hooks\n"
                  "pipe_hook(3)\n%quit\n", timeout=30)
        check("piped multiline hook runs", pipe.returncode == 0 and "(int) 10" in pipe.stdout)
        check("piped hooks prints the full multiline definition",
            "%mock pipe_hook(int x) cpp`\nreturn x+7;\n`" in pipe.stdout)
        pipe = subprocess.run([binary], cwd=cwd, text=True, capture_output=True,
            input="%mock missing() cpp`\nreturn 1;\n", timeout=30)
        check("EOF reports an unfinished hook", "unterminated cpp` block" in pipe.stdout)

        pipe = subprocess.run([binary], cwd=cwd, text=True, capture_output=True,
            input='#include <iostream>\n'
                  'int private_add(int a, int b) { return a+b; }\n'
                  'class PrivatePoint { void inc(int n) { private_add(n,1); } };\n'
                  '%pre private_add(int, int) in PrivatePoint::inc(int) cpp`std::cout << "called" << std::endl;`\n'
                  '%hooks\n%quit\n', timeout=30)
        check("stream hook can select an uncalled private caller",
            pipe.returncode == 0 and "%pre set:" in pipe.stdout and
            "%pre private_add(int, int) in PrivatePoint::inc(int) cpp`" in pipe.stdout and
            "error:" not in pipe.stderr)

    sys.exit(1 if failures else 0)


main()
