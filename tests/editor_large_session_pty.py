#!/usr/bin/env python3
"""Exercise the editor after loading a library and a symbol-heavy header."""
import os
from pathlib import Path
import pty
import re
import select
import signal
import sys
import tempfile
import time

binary, library = map(os.path.abspath, sys.argv[1:3])
prompt = b"cpp-repl\x1b[0m> "


def read_until(fd, needle, timeout=30):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], 0.05)[0]:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
        visible = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", output)
        if needle in output or needle in visible:
            return bytes(output)
    raise AssertionError(f"waiting for {needle!r}: {bytes(output)[-1500:]!r}")


with tempfile.TemporaryDirectory() as cwd:
    header = Path(cwd) / "large_editor.hpp"
    with header.open("w") as out:
        out.write("#pragma once\n#include <regex>\n#include <filesystem>\n#include <unordered_map>\n")
        for section in range(3000):
            out.write("namespace editor_large {\n")
            for member in range(10):
                out.write(f"int symbol_{section * 10 + member}(int value);\n")
            out.write("}\n")
        out.write("namespace editor_large { int sum(int lhs, int rhs) { return lhs + rhs; } }\n")

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.environ.update(HOME=cwd, TERM="xterm-256color")
        os.execv(binary, [binary])
    try:
        read_until(fd, prompt)
        for command in (f'%lib "{library}"', '#include "large_editor.hpp"'):
            os.write(fd, command.encode() + b"\r")
            output = read_until(fd, prompt)
            assert b"error:" not in output, output[-1500:]

        os.write(fd, b"editor_large::symbol_2999")
        read_until(fd, b"editor_large::symbol_2999")
        time.sleep(0.25)  # Include the idle coloring path before requesting Tab.
        started = time.monotonic()
        os.write(fd, b"\t\t")
        read_until(fd, b"symbol_29999", timeout=20)
        completion = time.monotonic() - started
        print(f"30,000-symbol header: completion menu {completion * 1000:.1f}ms", flush=True)
        assert completion < 2.0, f"completion stalled for {completion:.2f}s"
        os.write(fd, b"\x03")
        read_until(fd, prompt)

        os.write(fd, b"editor_large::su\t")
        read_until(fd, b"<placeholder>")
        # Typing after an idle pause used to queue behind synchronous analysis.
        started = time.monotonic()
        os.write(fd, b"2")
        read_until(fd, b"sum(2", timeout=2)
        echo = time.monotonic() - started
        print(f"large-session typing echo {echo * 1000:.1f}ms", flush=True)
        assert echo < 0.25, f"typing stalled for {echo:.2f}s"
        os.write(fd, b"0\t22\t\r")
        output = read_until(fd, prompt)
        assert b"(int) 42" in output and b"error:" not in output, output[-1500:]
        os.write(fd, b"%quit\r")
        # Keep draining output: macOS terminal mode restoration can wait for
        # pending output, so a blocking waitpid alone can deadlock the PTY.
        deadline = time.monotonic() + 5
        done, status = 0, None
        while time.monotonic() < deadline:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    os.read(fd, 65536)
                except OSError:
                    pass
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                break
        assert done, "REPL did not exit after %quit"
        assert status == 0, status
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.close(fd)
