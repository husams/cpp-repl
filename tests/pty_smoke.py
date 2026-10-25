#!/usr/bin/env python3
"""Drives cpp-repl through a pseudo-terminal.

Usage: pty_smoke.py <path-to-cpp-repl>
Runs in a scratch directory so the real history file is never touched.
"""
import os
import pty
import re
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import time


def session(binary, cwd, steps, home=None, args=()):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        # This test needs replxx's full terminal mode even when CTest's parent
        # shell uses TERM=dumb (as many automation runners do).
        os.environ["TERM"] = "xterm-256color"
        # Keep the user's actual ~/.cpp-repl out of test sessions.
        os.environ["HOME"] = home if home is not None else cwd
        os.execv(binary, [binary, *args])

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
        try:
            os.write(fd, keys)
        except OSError as error:
            previous = "\n".join(f"{label}: {plain(output)}" for label, output in list(outputs.items())[-3:])
            raise RuntimeError(f"REPL exited before {name}; recent output:\n{previous}") from error
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
    with tempfile.TemporaryDirectory() as shell_cwd:
        with open(os.path.join(shell_cwd, "find_lib.sh"), "w", encoding="utf8") as f:
            # Every toolchain can load the built test library; host libraries
            # such as libz may be outside a custom loader's search path.
            library = f'"{sys.argv[6]}"' if len(sys.argv) > 6 else "z"
            f.write(f"#!/bin/sh\nprintf '%s\\n' {shlex.quote(library)}\n")
        os.chmod(os.path.join(shell_cwd, "find_lib.sh"), 0o755)
        out, status = session(binary, shell_cwd, [
            ("shell_lib", b"%lib ``./find_lib.sh``\r", True),
            ("shell_open", b"%demangle ``\r"),
            ("shell_blank", b"\r"),
            ("shell_body", b"printf _Z3addii; printf x >> executed-once\r"),
            ("shell_close", b"``\r", True),
            ("shell_cancel", b"%help ``printf x > cancelled-shell\r"),
            ("shell_cancel_key", b"\x03", True),
            ("shell_flags", b"%add-compile-opt ``printf -- '-DSHELL_TEST_VALUE=73'``\r", True),
            ("shell_value", b"SHELL_TEST_VALUE\r", True),
            ("shell_fail", b"%quit ``exit 7``\r", True),
            ("clear_decl", b"int clear_preserved = 42;\r", True),
            ("clear", b"%clear\r", True),
            ("clear_value", b"clear_preserved\r", True),
            ("quit", b"%quit\r"),
        ])
        check("shell script output loads a library", "loaded:" in plain(out["shell_lib"]))
        check("open shell blocks accept blank lines before submission", all(
            "error:" not in plain(out[key]) for key in ("shell_open", "shell_blank", "shell_body")))
        check("multiline shell stdout is substituted", "add(int, int)" in plain(out["shell_close"]))
        with open(os.path.join(shell_cwd, "executed-once"), encoding="utf8") as f:
            check("editing and highlighting do not execute shell blocks", f.read() == "x")
        check("Ctrl+C cancels a shell block without running it",
              b"^C" in out["shell_cancel_key"] and
              not os.path.exists(os.path.join(shell_cwd, "cancelled-shell")))
        check("interpreter commands accept shell output", "(int) 73" in plain(out["shell_value"]))
        check("a failed shell prevents quit and leaves the session usable",
              "exit status 7" in plain(out["shell_fail"]) and status == 0)
        check("%clear clears the terminal and preserves C++ state",
              b"\x1b[2J\x1b[H" in out["clear"] and "(int) 42" in plain(out["clear_value"]))

        os.mkdir(os.path.join(shell_cwd, "sources dir"))
        with open(os.path.join(shell_cwd, "sources dir", "value.cpp"), "w", encoding="utf8") as f:
            f.write("int shell_file_value() { return 42; }\n")
        shell_env = dict(os.environ, HOME=shell_cwd, TERM="dumb")
        pipe = subprocess.run([binary], cwd=shell_cwd, env=shell_env, text=True,
            capture_output=True, input=
                '%add "``printf \'sources dir/value.cpp\'``"\n'
                'shell_file_value()\n'
                '%demangle ``\nprintf _Z3addii\n\n``\n'
                '%help ``cat``\n'
                '%demangle ``printf SHELL_STDERR >&2; printf _Z3addii``\n'
                '%quit ``exit 7``\n'
                '6 * 7\n%quit\n', timeout=30)
        check("piped commands preserve multiline shell output and quoted paths",
              pipe.returncode == 0 and "(int) 42" in pipe.stdout and
              pipe.stdout.count("add(int, int)") == 2)
        check("shell stdin cannot consume later REPL commands and stderr stays separate",
              "Commands:" in pipe.stdout and "SHELL_STDERR" in pipe.stderr and
              "SHELL_STDERR" not in pipe.stdout and "exit status 7" in pipe.stdout)

        os.mkdir(os.path.join(shell_cwd, ".cpp-repl"))
        startup = os.path.join(shell_cwd, ".cpp-repl", "0-shell.repl")
        with open(startup, "w", encoding="utf8") as f:
            f.write("%add-compile-opt ``\nprintf -- '-DSHELL_STARTUP_VALUE=91'\n``\n"
                    "%demangle ``printf '_Z3addii\\n'``\n")
        pipe = subprocess.run([binary], cwd=shell_cwd, env=shell_env, text=True,
            capture_output=True, input="SHELL_STARTUP_VALUE\n%quit\n", timeout=30)
        check("startup scripts support multiline shell substitution",
              pipe.returncode == 0 and "(int) 91" in pipe.stdout and
              "add(int, int)" in pipe.stdout and "error:" not in pipe.stdout + pipe.stderr)
        with open(startup, "w", encoding="utf8") as f:
            f.write("%lib ``printf z\n")
        pipe = subprocess.run([binary], cwd=shell_cwd, env=shell_env, text=True,
            capture_output=True, input="%quit\n", timeout=30)
        check("startup reports an unterminated shell block without executing it",
              "unterminated shell `` block" in pipe.stdout + pipe.stderr)

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
                ("demangle_tab", b"%dema\t"),
                ("demangle_linux", b" _Z3addii\r", True),
                ("demangle_mac", b"%demangle __ZN5Point3incEi\r", True),
                ("demangle_missing", b"%demangle\r", True),
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
        check("%help lists all commands", all(c in help_text for c in ("%lib", "%libs", "%libinfo", "%syminfo", "%demangle", "%define", "%paths", "%add-path", "%add", "%compile-opts", "%add-compile-opt", "%set-compile-opts", "%use-compile-opts", "%class", "%info", "%symbols", "%mock", "%pre", "%post", "%restore", "%hooks", "%clear", "%help", "%quit")))
        check("demangle completes and prints readable Linux and Mac C++ names",
              "%demangle" in plain(out["demangle_tab"])
              and "add(int, int)" in plain(out["demangle_linux"])
              and "Point::inc(int)" in plain(out["demangle_mac"]))
        check("demangle reports missing input with usage",
              "missing symbol name" in plain(out["demangle_missing"])
              and "usage: %demangle <symbol>" in plain(out["demangle_missing"]))
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
                ("methods_tab", b"%methods Po\t"),
                ("methods", b"\r", True),
                ("fields", b"%fields Point\r", True),
                ("access_tab", b"%methods Point access pub\t"),
                ("access", b"\r", True),
                ("parents_tab", b"%fields Point include-parents fal\t"),
                ("parents", b"\r", True),
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
        check("Tab completes a record name after %methods", "Point" in plain(out["methods_tab"]))
        check("%methods shows qualified names and types", "Point::sum : int () const" in plain(out["methods"]))
        check("%fields shows data members", all(m in plain(out["fields"]) for m in ("Point::x : int", "Point::y : int")))
        check("Tab completes access values", "public" in plain(out["access_tab"]) and "Point::sum" in plain(out["access"]))
        check("Tab completes parent values", "false" in plain(out["parents_tab"]) and "Point::x" in plain(out["parents"]))
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

        undo_example = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "examples", "undo-crash", "anonymous.cpp")
        out, status = session(binary, cwd, [
            ("add", ('%add "' + undo_example + '"\r').encode(), True),
            ("redefine", b"int undo_result = 7; int undo_local = 2;\r", True),
            ("result", b"undo_result + undo_local\r", True),
            ("quit", b"%quit\r"),
        ])
        check("%add reports an undefined symbol in an anonymous namespace",
            "error: undefined symbol: undo_missing" in plain(out["add"]))
        check("%add rollback returns a live prompt", PROMPT in out["add"])
        check("rejected declarations can be reused after %add rollback",
            "error:" not in plain(out["redefine"]) and "(int) 9" in plain(out["result"]))
        check("%quit exits after the former rollback crash", status == 0)

        for name, function in (("option file.cpp", "terminal_file_flag"),
                               ("other option.cpp", "terminal_other_flag")):
            with open(os.path.join(cwd, name), "w", encoding="utf8") as f:
                f.write("int " + function + "() { return TERM_FLAG; }\n")
        out, status = session(binary, cwd, [
            ("append", b"%add-compile-opt -std=c++20 -DTERM_FLAG=7\r", True),
            ("global_value", b"TERM_FLAG\r", True),
            ("set_file", b'%set-compile-opts "option file.cpp" -std=c++20 -DTERM_FLAG=42\r', True),
            ("file_tab", b'%compile-opts "option f\t'),
            ("file_options", b"\r", True),
            ("add_file", b'%add "option file.cpp"\r', True),
            ("add_other", b'%add "other option.cpp"\r', True),
            ("file_value", b"terminal_file_flag()\r", True),
            ("other_value", b"terminal_other_flag()\r", True),
            ("use_tab", b"%use-comp\t"),
            ("use_file", b' "option file.cpp"\r', True),
            ("reference_value", b"TERM_FLAG\r", True),
            ("overview", b"%compile-opts\r", True),
            ("replace", b"%set-compile-opts -DTERM_FLAG=9\r", True),
            ("bad", b"%add-compile-opt -fnot-a-valid-terminal-option\r", True),
            ("kept", b"TERM_FLAG\r", True),
            ("clear", b"%set-compile-opts\r", True),
            ("cleared", b"#ifdef TERM_FLAG\n#error flag leaked\n#endif\r", True),
            ("quit", b"%quit\r"),
        ])
        check("global compile flags affect interactive C++", "(int) 7" in plain(out["global_value"]))
        check("file compile flags override globals without affecting other files",
              "(int) 42" in plain(out["file_value"]) and "(int) 7" in plain(out["other_value"]))
        check("compile option filenames with spaces complete with Tab", "option file.cpp" in plain(out["file_tab"]))
        check("file compile options show effective flags with aligned fields",
              all(text in plain(out["file_options"]) for text in ("Scope", "Global flags", "File flags", "Effective flags", "TERM_FLAG=42")))
        if "NO_COLOR" not in os.environ:
            check("compile option fields are colored in terminals", b"\x1b[36m" in out["file_options"])
        check("bare compile options show global flags and file overrides", "File override" in plain(out["overview"]))
        check("use compile options completes and adopts file flags for the session",
              "%use-compile-opts" in plain(out["use_tab"]) and "(int) 42" in plain(out["reference_value"])
              and "Reference file" in plain(out["overview"]))
        check("invalid compiler flags preserve previous options", "error:" in plain(out["bad"]) and "(int) 9" in plain(out["kept"]))
        check("set compile options without flags clears globals", "error:" not in plain(out["cleared"]))
        check("compile option commands exit normally", status == 0)

        out, status = session(binary, cwd, [
            ("declare", b"namespace terminal_storage { int seed() { return 21; } class State { static long long value; public: static long long read() { return value; } static const char* label; }; }\r", True),
            ("undefined_tab", b"%symbols u\t"),
            ("undefined", b" name ^terminal_storage::State::\r", True),
            ("define_tab", b"%def\t"),
            ("bad", b" terminal_storage::State::value cpp`unknown_initializer`\r", True),
            ("define", b"%define terminal_storage::State::value cpp`\n\nseed() * 2LL\n`\r", True),
            ("label", b'%define terminal_storage::State::label cpp`"arbitrary text"`\r', True),
            ("value", b"int(terminal_storage::State::read())\r", True),
            ("text", b"terminal_storage::State::label\r", True),
            ("remaining", b"%symbols undefined name ^terminal_storage::State::\r", True),
            ("quit", b"%quit\r"),
        ])
        check("undefined symbol names complete and include missing private static members",
              "%symbols undefined" in plain(out["undefined_tab"])
              and "terminal_storage::State::value" in plain(out["undefined"])
              and "terminal_storage::State::label" in plain(out["undefined"]))
        check("define completes and accepts arbitrary typed multiline initializers after errors",
              "%define" in plain(out["define_tab"])
              and "error:" in plain(out["bad"])
              and "(int) 42" in plain(out["value"])
              and '(const char *) "arbitrary text"' in plain(out["text"]))
        check("defined statics disappear from the undefined list",
              "terminal_storage::State::value" not in plain(out["remaining"])
              and "terminal_storage::State::label" not in plain(out["remaining"])
              and status == 0)

        out, status = session(binary, cwd, [
            ("declare", b"namespace terminal_callable { class Worker { int value_ = 40; int hidden(int) const; public: int run(int x) const { return hidden(x); } }; }\r", True),
            ("bad", b"%define terminal_callable::Worker::hidden(int x) const cpp`return missing_name;`\r", True),
            ("define", b"%define terminal_callable::Worker::hidden(int x) const cpp`\n\nreturn this->value_ + x;\n`\r", True),
            ("call", b"terminal_callable::Worker{}.run(2)\r", True),
            ("quit", b"%quit\r"),
        ])
        check("callable define accepts multiline private method bodies after errors",
              status == 0 and "error:" in plain(out["bad"])
              and "error:" not in plain(out["define"]) and "(int) 42" in plain(out["call"]))

        person_header = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "fixtures", "newtag_person.hpp")
        out, status = session(binary, cwd, [
            ("header", f'#include "{person_header}"\r'.encode(), True),
            ("define", b'%define std::string Person::getName() const cpp`return "Sample";`\r', True),
            ("object", b"Person p;\r", True),
            ("call", b"p.getName();\r", True),
            ("capture", b"p.getName()\r", True),
            ("compare", b'p.getName() == "Sample"\r', True),
            ("quit", b"%quit\r"),
        ], args=("-fvisibility=hidden",))
        check("hidden visibility supports string-returning percent-define methods and value capture",
              status == 0 and "error:" not in plain(b"".join(out.values()))
              and "basic_string" in plain(out["capture"])
              and "(bool) true" in plain(out["compare"]))

        template_header = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "fixtures", "template_callables.hpp")
        out, status = session(binary, cwd, [
            ("header", f'#include "{template_header}"\r'.encode(), True),
            ("bad", b'%define std::string template_callables::describe<int>(int x) cpp`return unknown;`\r', True),
            ("function", b'%define std::string template_callables::describe<int>(int x) cpp`return std::to_string(x);`\r', True),
            ("method", b'%define std::string template_callables::Person<int>::name() const & noexcept cpp`\nreturn std::to_string(value_);\n`\r', True),
            ("specialization", b'%define std::string template_callables::Person<long>::name() const & noexcept cpp`return "specialized";`\r', True),
            ("objects", b'template_callables::Person<int> a{42}; template_callables::Person<long> b{7};\r', True),
            ("capture", b'a.name()\r', True),
            ("check", b'bool matches = a.name() == "42" && b.name() == "specialized" && template_callables::describe<int>(9) == "9";\r', True),
            ("value", b'matches\r', True),
            ("quit", b'%quit\r'),
        ], args=("-fvisibility=hidden",))
        check("percent-define supports template instances, declared specializations and retry",
              status == 0 and "error:" in plain(out["bad"])
              and all("error:" not in plain(value) for key, value in out.items() if key != "bad")
              and "basic_string" in plain(out["capture"])
              and "(bool) true" in plain(out["value"]))

        if len(sys.argv) > 3:
            with tempfile.TemporaryDirectory(dir=cwd) as callable_home:
                project = os.path.join(callable_home, "project")
                user_scripts = os.path.join(callable_home, ".cpp-repl")
                scripts = os.path.join(project, ".cpp-repl")
                nested = os.path.join(project, "nested")
                os.makedirs(user_scripts)
                os.makedirs(scripts)
                os.makedirs(nested)
                with open(os.path.join(user_scripts, "99-user.repl"), "w", encoding="utf8") as f:
                    f.write("int callable_startup_offset = 7;\n")
                with open(os.path.join(scripts, "1-header.repl"), "w", encoding="utf8") as f:
                    f.write(f'%add "{sys.argv[3]}"\n')
                with open(os.path.join(scripts, "2-definitions.repl"), "w", encoding="utf8") as f:
                    f.write('%define unresolved_callables::adjust(int x) cpp`return x + callable_startup_offset;`\n'
                            '%define unresolved_callables::adjust(double x) cpp`return x * 2.5;`\n'
                            '%define unresolved_callables::transform(unresolved_callables::Value v, double f) cpp`\nreturn {v.a*f, v.b*f, v.c*f, v.d*f};\n`\n'
                            '%define unresolved_callables::decorate(const std::string& s) cpp`return "[" + s + "]";`\n'
                            '%define unresolved_callables::Worker::hidden(int x) const & noexcept cpp`return value_ + x;`\n'
                            '%define unresolved_callables::Worker::scale(int x) cpp`return x*3;`\n')
                with open(os.path.join(scripts, "10-library.repl"), "w", encoding="utf8") as f:
                    f.write(f'%lib "{sys.argv[2]}"\n')
                out, status = session(binary, nested, [
                    ("probe", b"unresolved_callables::callable_native_probe()\r", True),
                    ("quit", b"%quit\r"),
                ], home=callable_home)
                check("ordered user/project startup supplies native callable imports before loading",
                      status == 0 and "error:" not in plain(out["start"])
                      and "(int) 1" in plain(out["probe"]))

        if len(sys.argv) > 5:
            with tempfile.TemporaryDirectory(dir=cwd) as virtual_home:
                project = os.path.join(virtual_home, "project")
                scripts = os.path.join(project, ".cpp-repl")
                nested = os.path.join(project, "nested")
                os.makedirs(scripts)
                os.makedirs(nested)
                with open(os.path.join(scripts, "1-header.repl"), "w", encoding="utf8") as f:
                    f.write(f'%add "{sys.argv[5]}"\n')
                with open(os.path.join(scripts, "2-definitions.repl"), "w", encoding="utf8") as f:
                    f.write('%define virtual_fixture::Service::compute(int x) const cpp`\nreturn seed_ + x;\n`\n'
                            '%define virtual_fixture::Implementation::compute(int x) const cpp`return bias_ + x*2;`\n'
                            '%define virtual_fixture::NativeTable::compute(int x) const cpp`return bias_ + x*3;`\n'
                            '%define virtual_fixture::VirtualBase::compute(int x) const cpp`return bias_ + x*4;`\n'
                            '%define virtual_fixture::Covariant::self() cpp`return this;`\n')
                with open(os.path.join(scripts, "10-library.repl"), "w", encoding="utf8") as f:
                    f.write(f'%lib "{sys.argv[4]}"\n')
                out, status = session(binary, nested, [
                    ("non_pure", b"virtual_fixture::virtual_non_pure_probe(2)\r", True),
                    ("override", b"virtual_fixture::virtual_override_probe(3)\r", True),
                    ("rtti", b"virtual_fixture::virtual_rtti_probe()\r", True),
                    ("virtual_base", b"virtual_fixture::virtual_base_probe(2)\r", True),
                    ("covariant", b"virtual_fixture::virtual_covariant_probe()\r", True),
                    ("quit", b"%quit\r"),
                ], home=virtual_home)
                check("startup supplies non-pure virtual methods, overrides, tables and thunks before native loading",
                      status == 0 and "error:" not in plain(out["start"])
                      and "(int) 42" in plain(out["non_pure"])
                      and "(int) 106" in plain(out["override"])
                      and "(int) 1" in plain(out["rtti"])
                      and "(int) 308" in plain(out["virtual_base"])
                      and "(int) 1" in plain(out["covariant"]))

        with tempfile.TemporaryDirectory(dir=cwd) as preload_home:
            with open(os.path.join(preload_home, "first header.hpp"), "w", encoding="utf8") as f:
                f.write("int startup_input_value = STARTUP_VALUE;\n")
            with open(os.path.join(preload_home, "reference.cpp"), "w", encoding="utf8") as f:
                f.write("int reference_should_not_load = 0;\n")
            with open(os.path.join(preload_home, "second.cpp"), "w", encoding="utf8") as f:
                f.write('extern "C" const char* zlibVersion();\n'
                        'int startup_answer() { return startup_input_value * 2; }\n'
                        'const char* startup_library_version() { return zlibVersion(); }\n')
            startup_dir = os.path.join(preload_home, ".cpp-repl")
            os.mkdir(startup_dir)
            with open(os.path.join(startup_dir, "0-options.repl"), "w", encoding="utf8") as f:
                f.write('// commands before files\n'
                        '%set-compile-opts reference.cpp -std=c++20 -DSTARTUP_VALUE=21\n'
                        '%use-compile-opts reference.cpp\n'
                        '%lib z\n%unknown-startup-command\n%add missing.cpp\n')
            with open(os.path.join(startup_dir, "2-header.repl"), "w", encoding="utf8") as f:
                f.write('#include <cstddef>\n%add "first header.hpp"\n')
            with open(os.path.join(startup_dir, "10-functions.repl"), "w", encoding="utf8") as f:
                f.write('%add second.cpp\n'
                        'int startup_script_sum() {\n\nreturn STARTUP_VALUE * 2;\n}\n')
            out, status = session(binary, preload_home, [
                ("call", b"startup_answer()\r", True),
                ("list", b"%add\r", True),
                ("options", b"%compile-opts\r", True),
                ("value", b"STARTUP_VALUE\r", True),
                ("library", b"startup_library_version()\r", True),
                ("script", b"startup_script_sum()\r", True),
                ("quit", b"%quit\r"),
            ])
            check("startup reads ~/.cpp-repl directory and continues after a missing entry",
                  "missing.cpp" in plain(out["start"]) and "preloaded:" in plain(out["start"])
                  and "(int) 42" in plain(out["call"]))
            check("preloaded source files appear in the session", "second.cpp" in plain(out["list"]) and status == 0)
            check("startup commands configure session defaults before subsequent files",
                  "Reference file" in plain(out["options"])
                  and "(int) 21" in plain(out["value"])
                  and "reference.cpp" not in plain(out["list"]))
            check("startup command errors include configuration line and later entries run",
                  "0-options.repl:5:" in plain(out["start"])
                  and "unknown command" in plain(out["start"])
                  and "(int) 42" in plain(out["call"]))
            check("startup libraries remain available to preloaded C++",
                  '(const char *) "1.' in plain(out["library"]))
            check("numbered startup scripts execute in numeric order with multiline C++",
                  "2-header.repl" in plain(out["start"])
                  and "10-functions.repl" in plain(out["start"])
                  and "(int) 42" in plain(out["script"]))

        with tempfile.TemporaryDirectory(dir=cwd) as quit_home:
            os.mkdir(os.path.join(quit_home, ".cpp-repl"))
            with open(os.path.join(quit_home, ".cpp-repl", "0-quit.repl"), "w", encoding="utf8") as f:
                f.write("%quit\n%add missing-after-quit.cpp\n")
            quit_run = subprocess.run(
                [binary], cwd=quit_home, input="int should_not_run = 1;\n",
                text=True, capture_output=True, timeout=30,
                env={**os.environ, "HOME": quit_home})
            check("startup quit exits before the prompt or later entries",
                  quit_run.returncode == 0 and "missing-after-quit" not in quit_run.stdout
                  and "cpp-repl>" not in plain(quit_run.stdout.encode()))

        with tempfile.TemporaryDirectory(dir=cwd) as scope_home:
            user_scripts = os.path.join(scope_home, ".cpp-repl")
            project_root = os.path.join(scope_home, "project")
            project_scripts = os.path.join(project_root, ".cpp-repl")
            nested = os.path.join(project_root, "src", "nested")
            os.makedirs(user_scripts)
            os.makedirs(project_scripts)
            os.makedirs(nested)
            with open(os.path.join(user_scripts, "99-user.repl"), "w", encoding="utf8") as f:
                f.write("%set-compile-opts -DSCOPE_VALUE=1\nint user_scope_value = SCOPE_VALUE;\n")
            with open(os.path.join(project_scripts, "0-project.repl"), "w", encoding="utf8") as f:
                f.write("%add-compile-opt -DSCOPE_VALUE=2\n%add src/project.cpp\n")
            with open(os.path.join(project_root, "src", "project.cpp"), "w", encoding="utf8") as f:
                f.write("int project_scope_value() { return user_scope_value + SCOPE_VALUE; }\n")
            out, status = session(binary, nested, [
                ("user", b"user_scope_value\r", True),
                ("project", b"project_scope_value()\r", True),
                ("quit", b"%quit\r"),
            ], home=scope_home)
            check("user startup runs before nearest project startup even from a subdirectory",
                  status == 0 and "error:" not in plain(out["start"])
                  and "(int) 1" in plain(out["user"])
                  and "(int) 3" in plain(out["project"]))

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
            ("paths_tab", b"%pat\t"),
            ("paths", b"\r", True),
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
        check("paths completes and reports loader settings",
            "%paths" in plain(out["paths_tab"]) and all(text in plain(out["paths"]) for text in
                ("Library search paths:", "Added", "Dependency fallback", "Native loader")))
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


if __name__ == "__main__":
    main()
