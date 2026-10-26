#!/usr/bin/env python3
"""Keep semantic AST analysis off the synchronous per-key editor path."""
import os
import pty
import re
import statistics
import select
import signal
import sys
import tempfile
import time
import errno

binary = os.path.abspath(sys.argv[1])
PROMPT = b"cpp-repl\x1b[0m> "


def read_until(fd, needle, timeout=45):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            data.extend(os.read(fd, 65536))
        except OSError:
            break
        if needle in data:
            return bytes(data)
    raise AssertionError(f"timed out waiting for {needle!r}; got {bytes(data)[-500:]!r}")


with tempfile.TemporaryDirectory() as cwd:
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.environ.update(HOME=cwd, TERM="xterm-256color")
        os.execv(binary, [binary])
    try:
        read_until(fd, PROMPT)
        os.write(fd, b"#include <iostream>\r")
        read_until(fd, PROMPT)
        os.write(fd, b"int editor_latency_probe(std::string value, int count) { return count; }\r")
        read_until(fd, PROMPT)
        os.write(fd, b"editor_latency_probe(")
        # Let the previous idle analysis finish before measuring a typing burst.
        read_until(fd, b"[argument 1]")
        while select.select([fd], [], [], 0)[0]:
            os.read(fd, 65536)
        # Drain any pending terminal output before timing. Every character
        # changes an open call's completion/signature context and used to run
        # the synchronous AST path (about 191-230ms per key in the review build).
        latencies = []
        typed = b""
        for key in b"std::str":
            typed += bytes([key])
            expected = b"editor_latency_probe(" + typed
            started = time.monotonic()
            os.write(fd, bytes([key]))
            output = bytearray()
            deadline = started + 2.0
            rendered = False
            while time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
                if not ready:
                    break
                try:
                    output.extend(os.read(fd, 65536))
                except OSError:
                    break
                visible = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", output)
                if expected in visible:
                    rendered = True
                    break
            assert rendered, f"editor did not render {expected!r}"
            latencies.append(time.monotonic() - started)
        os.write(fd, b"\x03")
        read_until(fd, PROMPT, timeout=10)
        os.write(fd, b"%quit\r")
        read_until(fd, b"%quit", timeout=10)
        deadline = time.monotonic() + 5
        status = None
        while time.monotonic() < deadline:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                break
            time.sleep(0.02)
        if status is None or not done:
            raise AssertionError("REPL did not exit after %quit")
        worst = max(latencies)
        median = statistics.median(latencies)
        print(f"per-key echo wait: median={median * 1000:.1f}ms, max={worst * 1000:.1f}ms; samples=" +
              ",".join(f"{sample * 1000:.1f}" for sample in latencies) + "ms")
        assert median < 0.08, f"editor repeatedly blocked while typing (median {median * 1000:.1f}ms)"
        assert sum(sample >= 0.18 for sample in latencies) <= 2, "repeated slow editor redraws"
        assert status == 0, f"REPL exited abnormally: {status}"
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.close(fd)
