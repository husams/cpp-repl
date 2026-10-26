#!/usr/bin/env python3
"""Compare REPL editor responsiveness in a large C++ session using a real PTY.

Run once for each binary with the same arguments, for example:
  python3 tests/editor_large_context_benchmark.py BASELINE LIBRARY --skip-menu --json-out baseline.json
  python3 tests/editor_large_context_benchmark.py CURRENT LIBRARY --expect-menu --json-out current.json

The fixture is deterministic (its SHA-256 is included in the JSON), so separate
baseline/current runs use byte-identical headers without sharing mutable REPL
state. Only Python's standard library is required.
"""
import argparse
import hashlib
import json
import os
import pty
import re
import select
import signal
import statistics
import sys
import tempfile
import time
from pathlib import Path


PROMPT = b"cpp-repl> "
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
FIELD_COLOR = b"\x1b[0;22;36mvalue"


def visible(data):
    return ANSI.sub(b"", data)


def make_fixture(path, declarations):
    reopenings = (declarations + 9) // 10
    output = [
        "#pragma once\n#include <regex>\n#include <filesystem>\n#include <unordered_map>\n"
    ]
    for section in range(reopenings):
        output.append("namespace perfbench {\n")
        for member in range(10):
            index = section * 10 + member
            if index >= declarations:
                break
            output.append(f"int symbol_{index}(int value);\n")
        output.append("}\n")
    output.append("""
namespace perfbench {
struct BenchmarkObject { int value = 37; };
inline BenchmarkObject benchmark_object;
inline int unfinished(int first, int second) { return first + second; }
inline int zero() { return 21; }
}
inline int benchmark_overloaded(int first) { return first + 1; }
inline int benchmark_overloaded(int first, int second) { return first + second; }
""")
    data = "".join(output).encode()
    path.write_bytes(data)
    return {
        "declarations": declarations,
        "namespace_reopenings": reopenings,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


class PtySession:
    def __init__(self, binary, cwd):
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(cwd)
            os.environ.update(HOME=cwd, TERM="xterm-256color")
            os.execv(binary, [binary])
        self.pid = pid
        self.fd = fd
        self.closed = False

    def read_until(self, needle, timeout=30.0, raw=False):
        output = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], min(0.1, deadline - time.monotonic()))
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            output.extend(chunk)
            haystack = bytes(output) if raw else visible(output)
            if needle in haystack:
                return bytes(output)
        tail = bytes(output[-1200:])
        raise AssertionError(f"timed out waiting for {needle!r}; output tail={tail!r}")

    def wait_activity(self, timeout=3.0):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            raise AssertionError("no PTY redraw after input")
        try:
            return os.read(self.fd, 65536)
        except OSError as error:
            raise AssertionError("PTY closed before redraw") from error

    def drain(self, quiet=0.04):
        chunks = bytearray()
        while select.select([self.fd], [], [], quiet)[0]:
            try:
                chunks.extend(os.read(self.fd, 65536))
            except OSError:
                break
        return bytes(chunks)

    def send(self, data):
        os.write(self.fd, data)

    def close(self):
        if self.closed:
            return
        try:
            self.send(b"%quit\r")
        except OSError:
            pass
        deadline = time.monotonic() + 4.0
        done = 0
        status = None
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], 0.03)[0]:
                try:
                    os.read(self.fd, 65536)
                except OSError:
                    pass
            done, status = os.waitpid(self.pid, os.WNOHANG)
            if done:
                break
            time.sleep(0.01)
        if not done:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(self.pid, os.WNOHANG)
        os.close(self.fd)
        self.closed = True
        if not done:
            raise AssertionError("REPL did not exit cleanly after %quit")
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            raise AssertionError(f"REPL exited abnormally: {status}")


def timed_write(session, data, needle, timeout=30.0, raw=False):
    session.drain()
    started = time.perf_counter_ns()
    session.send(data)
    output = session.read_until(needle, timeout, raw=raw)
    return (time.perf_counter_ns() - started) / 1_000_000, output


def summarize(samples):
    if not samples:
        return None
    return {
        "samples_ms": [round(x, 3) for x in samples],
        "p50_ms": round(statistics.median(samples), 3),
        "max_ms": round(max(samples), 3),
    }


def benchmark(binary, library, iterations, declarations, expect_menu, skip_menu):
    with tempfile.TemporaryDirectory(prefix="cpp-repl-large-context-") as cwd:
        header_path = Path(cwd) / "large_context.hpp"
        fixture = make_fixture(header_path, declarations)
        session = PtySession(binary, cwd)
        metrics = {}
        validation = {}
        try:
            session.read_until(PROMPT)
            session.send(f'%lib "{library}"\r'.encode())
            lib_output = session.read_until(PROMPT)
            validation["native_library_loaded"] = (
                b"error:" not in visible(lib_output) and Path(library).name.encode() in lib_output
            )
            if not validation["native_library_loaded"]:
                raise AssertionError(f"native test library did not load: {visible(lib_output)[-800:]!r}")
            session.send(b'#include "large_context.hpp"\r')
            include_output = session.read_until(PROMPT, timeout=90.0)
            if b"error:" in visible(include_output):
                raise AssertionError(f"large fixture failed to load: {visible(include_output)[-1200:]!r}")

            completion_samples = []
            for sample in range(iterations):
                target = declarations - 1 - sample * 10
                prefix = target // 10
                elapsed, output = timed_write(
                    session,
                    f"perfbench::symbol_{prefix}\t".encode(),
                    f"symbol_{target}".encode(),
                    timeout=60.0,
                )
                completion_samples.append(elapsed)
                validation["qualified_completion_found"] = (
                    validation.get("qualified_completion_found", True) and
                    f"symbol_{target}".encode() in visible(output)
                )
                session.send(b"\x03")
                session.read_until(PROMPT)
            metrics["qualified_completion_ms"] = summarize(completion_samples)

            key_samples = []
            typed = b""
            for _ in range(iterations):
                session.send(b"int perfbench_typing_probe_")
                session.read_until(b"int perfbench_typing_probe_")
                typed = b""
                for key in b"abcdefghij":
                    typed += bytes([key])
                    elapsed, _ = timed_write(
                        session, bytes([key]), b"int perfbench_typing_probe_" + typed, timeout=5.0
                    )
                    key_samples.append(elapsed)
                session.send(b"\x03")
                session.read_until(PROMPT)
            metrics["ordinary_key_echo_ms"] = summarize(key_samples)

            color_samples = []
            for sample in range(iterations):
                color_input = b"perfbench::benchmark_object.value /* sample " + str(sample).encode() + b" */"
                elapsed, color_output = timed_write(
                    session,
                    color_input,
                    FIELD_COLOR,
                    timeout=10.0,
                    raw=True,
                )
                color_samples.append(elapsed)
                validation["idle_semantic_field_color"] = (
                    validation.get("idle_semantic_field_color", True) and
                    FIELD_COLOR in color_output
                )
                session.send(b"\x03")
                session.read_until(PROMPT)
            metrics["idle_semantic_color_ms"] = summarize(color_samples)

            embedded_evaluation = []
            for sample in range(iterations):
                source = (
                    f"perfbench::unfinished(20,22)+0 /* enter sample {sample} */;".encode()
                )
                session.send(source)
                session.read_until(source, timeout=10.0)
                session.drain()
                elapsed, output = timed_write(session, b"\r", PROMPT, timeout=60.0)
                embedded_evaluation.append(elapsed)
                validation["embedded_evaluation_value_preserved"] = (
                    validation.get("embedded_evaluation_value_preserved", True) and
                    b"(int) 42" in visible(output)
                )
                if not validation["embedded_evaluation_value_preserved"]:
                    raise AssertionError(
                        f"embedded evaluation changed value: {visible(output)[-800:]!r}"
                    )
            metrics["embedded_evaluation_ms"] = summarize(embedded_evaluation)

            for metric, expression in (
                (
                    "multi_call_evaluation_ms",
                    "perfbench::unfinished(10,11)+perfbench::unfinished(10,11)",
                ),
                (
                    "nested_call_evaluation_ms",
                    "perfbench::unfinished(perfbench::unfinished(10,11),0)"
                    "+perfbench::unfinished(10,11)",
                ),
                (
                    "manual_zero_call_multi_evaluation_ms",
                    "perfbench::zero()+perfbench::zero()",
                ),
            ):
                samples = []
                for sample in range(iterations):
                    source = f"{expression} /* {metric} sample {sample} */;".encode()
                    session.send(source)
                    session.read_until(source, timeout=10.0)
                    session.drain()
                    elapsed, output = timed_write(session, b"\r", PROMPT, timeout=60.0)
                    samples.append(elapsed)
                    validation[metric + "_value_preserved"] = (
                        validation.get(metric + "_value_preserved", True) and
                        b"(int) 42" in visible(output)
                    )
                    if not validation[metric + "_value_preserved"]:
                        raise AssertionError(
                            f"{metric} changed value: {visible(output)[-800:]!r}"
                        )
                metrics[metric] = summarize(samples)

            move_samples, edit_samples = [], []
            for _ in range(iterations):
                session.send(b"perfbench::unfinished(")
                session.read_until(b"first: ", timeout=10.0)
                elapsed, _ = timed_write(session, b"1", b"unfinished(1", timeout=5.0)
                edit_samples.append(elapsed)
                session.drain()
                started = time.perf_counter_ns()
                session.send(b"\x1b[D")
                session.wait_activity()
                move_samples.append((time.perf_counter_ns() - started) / 1_000_000)
                elapsed, moved = timed_write(session, b"2", b"unfinished(21", timeout=5.0)
                edit_samples.append(elapsed)
                validation["unfinished_call_edit_visible"] = (
                    validation.get("unfinished_call_edit_visible", True) and
                    b"unfinished(21" in visible(moved)
                )
                session.send(b"\x03")
                session.read_until(PROMPT)
            metrics["unfinished_call_cursor_move_ms"] = summarize(move_samples)
            metrics["unfinished_call_edit_ms"] = summarize(edit_samples)

            menu_open, menu_move, menu_accept = [], [], []
            slot_tab, argument_type = [], []
            menu_supported = False
            menu_timeouts = 0
            for _ in range(0 if skip_menu else iterations):
                session.send(b"benchmark_overloaded(")
                # Wait for the lexical source echo only. Waiting for the first
                # semantic hint would warm the completion cache before timing
                # the first picker opening.
                session.read_until(b"benchmark_overloaded(", timeout=5.0)
                session.drain()
                started = time.perf_counter_ns()
                session.send(b"\t")
                try:
                    first_output = session.read_until(b"> benchmark_overloaded(", timeout=8.0)
                except AssertionError:
                    menu_supported = False
                    menu_timeouts += 1
                    session.send(b"\x03")
                    session.read_until(PROMPT)
                    break
                menu_supported = True
                menu_open.append((time.perf_counter_ns() - started) / 1_000_000)
                visible_first = visible(first_output).decode("utf8", "replace")
                rows = [line for line in visible_first.replace("\r", "").splitlines()
                        if line.startswith(("> benchmark_overloaded(", "  benchmark_overloaded("))]
                if len(rows) < 2:
                    raise AssertionError(f"overload menu did not show both rows: {visible_first[-700:]!r}")
                second_row = rows[1].strip()
                validation["menu_shows_overloads"] = (
                    validation.get("menu_shows_overloads", True) and len(rows) >= 2
                )

                session.drain()
                started = time.perf_counter_ns()
                session.send(b"\x1b[B")
                moved = session.read_until(b"> " + second_row.encode(), timeout=5.0)
                menu_move.append((time.perf_counter_ns() - started) / 1_000_000)
                validation["menu_navigation_changes_selection"] = (
                    validation.get("menu_navigation_changes_selection", True) and
                    b"> " + second_row.encode() in visible(moved)
                )

                started = time.perf_counter_ns()
                session.send(b"\t")
                accepted = session.read_until(b"second: ", timeout=5.0)
                menu_accept.append((time.perf_counter_ns() - started) / 1_000_000)
                validation["selected_parameters_visible"] = (
                    validation.get("selected_parameters_visible", True) and
                    b"second:" in visible(accepted)
                )

                session.send(b"7")
                session.read_until(b"benchmark_overloaded(7")
                elapsed, slot_output = timed_write(session, b"\t", b"benchmark_overloaded(7, ", timeout=5.0)
                slot_tab.append(elapsed)
                validation["parameter_tab_stop_visible"] = (
                    validation.get("parameter_tab_stop_visible", True) and
                    b"benchmark_overloaded(7, " in visible(slot_output)
                )
                elapsed, argument_output = timed_write(session, b"8", b"benchmark_overloaded(7, 8", timeout=5.0)
                argument_type.append(elapsed)
                session.send(b"\r")
                result_output = session.read_until(PROMPT, timeout=30.0)
                validation["selected_overload_executes"] = (
                    validation.get("selected_overload_executes", True) and
                    b"(int) 15" in visible(result_output)
                )
                if not validation["selected_overload_executes"]:
                    raise AssertionError(f"overload acceptance changed evaluation: {visible(result_output)[-800:]!r}")

            if menu_supported:
                metrics["overload_menu_open_ms"] = summarize(menu_open)
                metrics["overload_menu_first_open_ms"] = round(menu_open[0], 3)
                metrics["overload_menu_subsequent_open_ms"] = summarize(menu_open[1:])
                metrics["overload_menu_navigation_warm_ms"] = summarize(menu_move)
                metrics["overload_menu_accept_warm_ms"] = summarize(menu_accept)
                metrics["parameter_tab_stop_warm_ms"] = summarize(slot_tab)
                metrics["parameter_typing_warm_ms"] = summarize(argument_type)
            else:
                metrics["overload_menu_open_ms"] = None
                metrics["overload_menu_first_open_ms"] = None
                metrics["overload_menu_subsequent_open_ms"] = None
                metrics["overload_menu_navigation_warm_ms"] = None
                metrics["overload_menu_accept_warm_ms"] = None
                metrics["parameter_tab_stop_warm_ms"] = None
                metrics["parameter_typing_warm_ms"] = None
            validation["menu_supported"] = menu_supported
            validation["menu_detection_timeouts"] = menu_timeouts
            if expect_menu and not menu_supported:
                raise AssertionError("expected overload picker, but no menu was rendered")

            session.send(b"perfbench::benchmark_object.value\r")
            value_output = session.read_until(PROMPT)
            validation["evaluation_value_preserved"] = b"(int) 37" in visible(value_output)
            session.close()
            history_file = Path(cwd) / ".replxx_history"
            history = history_file.read_text(encoding="utf8") if history_file.exists() else ""
            validation["history_excludes_display_decorations"] = not any(
                marker in history
                for marker in ("benchmark_overloaded(first:", "benchmark_overloaded(second:",
                               " -> int", "> benchmark_overloaded(")
            )
            required = ["native_library_loaded", "qualified_completion_found",
                        "idle_semantic_field_color", "unfinished_call_edit_visible",
                        "evaluation_value_preserved", "history_excludes_display_decorations"]
            if menu_supported:
                required.extend(["menu_shows_overloads", "menu_navigation_changes_selection",
                                 "selected_parameters_visible", "parameter_tab_stop_visible",
                                 "selected_overload_executes"])
            failed = [name for name in required if not validation.get(name, False)]
            if failed:
                raise AssertionError("output/history validation failed: " + ", ".join(failed))
        finally:
            session.close()

    return {
        "schema_version": 1,
        "binary": binary,
        "native_library": library,
        "iterations": iterations,
        "fixture": fixture,
        "metrics": metrics,
        "validation": validation,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="cpp-repl executable to benchmark")
    parser.add_argument("native_library", help="existing native test library to load")
    parser.add_argument("--iterations", type=int, default=3, help="samples per operation (default: 3)")
    parser.add_argument("--stress", action="store_true", help="use 100,000 declarations instead of 30,000")
    parser.add_argument("--expect-menu", action="store_true", help="fail if overload-menu features are absent")
    parser.add_argument("--skip-menu", action="store_true", help="skip current-only picker metrics for a baseline")
    parser.add_argument("--json-out", help="also save the JSON report to this path")
    args = parser.parse_args()
    if args.iterations < 1 or args.iterations > 20:
        parser.error("--iterations must be between 1 and 20")
    if args.skip_menu and args.expect_menu:
        parser.error("--skip-menu and --expect-menu cannot be combined")
    binary = os.path.abspath(args.binary)
    library = os.path.abspath(args.native_library)
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        parser.error(f"binary is not executable: {binary}")
    if not os.path.isfile(library):
        parser.error(f"native library does not exist: {library}")
    report = benchmark(binary, library, args.iterations,
                       100_000 if args.stress else 30_000, args.expect_menu, args.skip_menu)
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.json_out:
        Path(args.json_out).write_text(rendered + "\n", encoding="utf8")
    print(rendered)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, TimeoutError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
