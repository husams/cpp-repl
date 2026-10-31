#!/usr/bin/env python3
"""Paused ordinary typing stays responsive with a large header and call hints."""
import fcntl
import os
import pty
import re
import select
import signal
import statistics
import struct
import sys
import tempfile
import termios
import time


binary = os.path.abspath(sys.argv[1])
PROMPT = b"cpp-repl> "
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def visible(data):
    return ANSI.sub(b"", data)


def read_until(fd, needle, timeout=180):
    needles = (needle,) if isinstance(needle, bytes) else tuple(needle)
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0.0, min(0.1, deadline - time.monotonic())))
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
        rendered = visible(output)
        if all(item in rendered for item in needles):
            return bytes(output)
    raise AssertionError(f"timed out waiting for {needles!r}; tail={bytes(output[-1200:])!r}")


def drain(fd, quiet=0.01):
    output = bytearray()
    while select.select([fd], [], [], quiet)[0]:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


with tempfile.TemporaryDirectory(prefix="cpp-repl-typing-lag-") as cwd:
    header = os.path.join(cwd, "typing_lag.hpp")
    with open(header, "w", encoding="utf8") as fixture:
        fixture.write("#pragma once\n#include <algorithm>\n")
        fixture.writelines(f"int typing_lag_unused_{i}(int);\n" for i in range(50000))
    startup_dir = os.path.join(cwd, ".cpp-repl")
    os.mkdir(startup_dir)
    with open(os.path.join(startup_dir, "10-typing-lag.repl"), "w", encoding="utf8") as startup:
        startup.write('#include "typing_lag.hpp"\n')

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        cache = os.path.join(cwd, "xdg-cache")
        os.makedirs(cache, exist_ok=True)
        os.environ.update(HOME=cwd, TERM="xterm-256color", XDG_CACHE_HOME=cache)
        os.execv(binary, [binary])

    reaped = False
    status = None
    try:
        # Match the measured terminal geometry so call annotations do not wrap.
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 240, 0, 0))
        initial = read_until(fd, PROMPT)
        assert b"error:" not in visible(initial), visible(initial)[-1200:]

        marker = b"typing_probe_0_8_200_"
        base = b"int value = " + b"".join(
            f"std::max({i + 1}, {i + 2}) + ".encode() for i in range(8)
        ) + marker
        os.write(fd, base)
        transcript = bytearray(read_until(fd, marker))
        # Let the initial eight-call hint refresh finish, then discard its
        # pending redraw before timing key echoes.
        time.sleep(2.0)
        transcript.extend(drain(fd, quiet=0.01))

        latencies = []
        typed = b""
        for key in b"abcdefgh":
            # Pause after the preceding echo; the first key follows the base
            # line's two-second idle settle plus this same 200 ms pause.
            time.sleep(0.2)
            typed += bytes([key])
            expected = marker + typed
            started = time.perf_counter()
            os.write(fd, bytes([key]))
            data = read_until(fd, expected, timeout=10)
            latencies.append(time.perf_counter() - started)
            transcript.extend(data)

        # This untimed distinctive suffix forces a fresh hint redraw after
        # timing and keeps the latest line distinguishable from the old one.
        os.write(fd, b"x")
        verification = read_until(fd, (marker + typed + b"x", b"__a:", b"__b:"), timeout=10)
        transcript.extend(verification)
        assert marker + typed + b"x" in visible(verification), "latest edited line marker disappeared"

        os.write(fd, b"\x03")
        read_until(fd, PROMPT, timeout=10)
        os.write(fd, b"6*7\r")
        result = read_until(fd, PROMPT, timeout=30)
        assert b"(int) 42" in visible(result), "REPL did not evaluate the follow-up expression"

        slow = sum(sample >= 0.20 for sample in latencies)
        median = statistics.median(latencies)
        print(f"paused key echo: median={median * 1000:.1f}ms, >=200ms={slow}/8")
        assert median < 0.15 and slow <= 2, "ordinary typing repeatedly blocked after idle pauses"

        os.write(fd, b"%quit\r")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            drain(fd, quiet=0.05)
            done, wait_status = os.waitpid(pid, os.WNOHANG)
            if done:
                reaped = True
                status = wait_status
                break
        assert reaped and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, \
            f"REPL did not exit normally: reaped={reaped}, status={status}"
    finally:
        if not reaped:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
        os.close(fd)
