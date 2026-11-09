#!/usr/bin/env python3
"""Local test driver for MiniVectorDB.

The driver starts the database with an empty data directory, feeds the SQL of a
test case through stdin and compares the captured stdout with the expected
result file.  Two directives are understood inside a test case:

  -- !kill      the process is killed with SIGKILL and restarted with the same
                data directory, the remaining statements are executed by the
                new process (crash recovery).
  -- !stop      the process is stopped gracefully (stdin closed) and restarted
                with the same data directory (normal shutdown).

Usage:
  python3 tests/run_tests.py [--binary PATH] [--update] [pattern ...]
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")


def split_segments(text):
    """Splits a test case into segments separated by the restart directives."""
    segments = []
    current = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == "-- !kill":
            segments.append(("kill", "".join(current)))
            current = []
        elif stripped == "-- !stop":
            segments.append(("stop", "".join(current)))
            current = []
        else:
            current.append(line)
    segments.append(("run", "".join(current)))
    return segments


def run_case(binary, case_path, update=False):
    name = os.path.basename(case_path)
    sql_path = case_path if name.endswith(".sql") else case_path + ".sql"
    expected_path = os.path.splitext(sql_path)[0] + ".expected"
    with open(sql_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    options = []
    for line in text.splitlines():
        if line.strip().startswith("-- @options"):
            for item in line.split(":", 1)[1].split():
                if "=" in item:
                    key, value = item.split("=", 1)
                    options.append("--" + key.strip() + "=" + value.strip())
            break
    segments = split_segments(text)

    workdir = tempfile.mkdtemp(prefix="mvdb-test-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir)
    output = []

    def spawn():
        process = subprocess.Popen(
            [binary, "--data-dir", data_dir] + options,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        collected = []

        def reader():
            fd = process.stdout.fileno()
            while True:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                collected.append(chunk)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        return process, collected, thread

    try:
        process, collected, thread = spawn()
        for action, payload in segments:
            if payload.strip():
                process.stdin.write(payload.encode("utf-8"))
                process.stdin.flush()
            if action in ("kill", "stop"):
                # Synchronise with the server: the marker query has to be
                # answered before the process is terminated, otherwise the
                # test would race with the statement execution.
                marker = "SELECT '__mvdb_sync__';\n"
                process.stdin.write(marker.encode("utf-8"))
                process.stdin.flush()
                deadline = time.time() + 30
                while time.time() < deadline:
                    if b"__mvdb_sync__" in b"".join(collected):
                        break
                    time.sleep(0.01)
            if action == "kill":
                process.stdin.flush()
                process.send_signal(signal.SIGKILL)
                process.wait(timeout=30)
                thread.join(timeout=10)
                output.append(b"".join(collected).decode("utf-8", "replace"))
                process, collected, thread = spawn()
                continue
            if action == "stop":
                process.stdin.close()
                process.wait(timeout=30)
                thread.join(timeout=10)
                output.append(b"".join(collected).decode("utf-8", "replace"))
                process, collected, thread = spawn()
                continue
        process.stdin.close()
        returncode = process.wait(timeout=60)
        thread.join(timeout=10)
        output.append(b"".join(collected).decode("utf-8", "replace"))
        if returncode != 0:
            return False, "the server exited with code %d" % returncode
    finally:
        try:
            if process.poll() is None:
                process.kill()
        except Exception:
            pass
        shutil.rmtree(workdir, ignore_errors=True)

    actual = "".join(output)
    if update:
        with open(expected_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(actual)
        return True, "updated"

    if not os.path.exists(expected_path):
        return False, "missing expected file %s" % os.path.basename(expected_path)
    with open(expected_path, "r", encoding="utf-8") as handle:
        expected = handle.read()
    if expected != actual:
        expected_lines = expected.splitlines()
        actual_lines = actual.splitlines()
        detail = []
        for index in range(max(len(expected_lines), len(actual_lines))):
            want = expected_lines[index] if index < len(expected_lines) else "<missing>"
            got = actual_lines[index] if index < len(actual_lines) else "<missing>"
            if want != got:
                detail.append("  line %d:\n    expected: %s\n    actual:   %s" % (index + 1, want, got))
            if len(detail) >= 8:
                break
        return False, "\n".join(detail)
    return True, ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=os.environ.get("MINIVECTORDB_BINARY", ""))
    parser.add_argument("--update", action="store_true")
    parser.add_argument("patterns", nargs="*")
    args = parser.parse_args()

    binary = args.binary
    if not binary:
        candidate = os.path.join(HERE, "..", "build", "minivectordb")
        binary = os.path.abspath(candidate)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    if not os.path.isdir(CASES):
        print("no test cases in %s" % CASES)
        return 2
    cases = sorted(f for f in os.listdir(CASES) if f.endswith(".sql"))
    if args.patterns:
        cases = [c for c in cases if any(p in c for p in args.patterns)]

    failures = 0
    for case in cases:
        ok, detail = run_case(binary, os.path.join(CASES, case), args.update)
        if ok:
            print("PASS %s %s" % (case, detail))
        else:
            failures += 1
            print("FAIL %s\n%s" % (case, detail))
    print("%d case(s), %d failure(s)" % (len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())