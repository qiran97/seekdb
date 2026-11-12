#!/usr/bin/env python3
"""Runs the public MySQL cases with the protocol checker of the task package.

The checker in `tests/protocol_check` is the one of the task package: it
speaks the JSON lines protocol over the standard input and the standard
output of the database and compares the answers with the expected results
of the public cases in `tests/mysqltest`.  The database is started as a
local process here, the checker starts a container when an image is given.

Usage:
  python3 tests/protocol_check.py [--binary PATH]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(ROOT, "protocol_check", "run.py")
PACKAGE = os.path.join(ROOT, "mysqltest")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--case", action="append", default=None)
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2
    if not os.path.exists(CHECKER):
        print("the protocol checker is not there: %s" % CHECKER)
        return 2

    # The checker wants a directory that does not exist yet.
    root = tempfile.mkdtemp(prefix="mvdb-protocol-")
    output = os.path.join(root, "report")
    command = json.dumps([binary, "--data-dir", "{data_dir}"])
    argv = [sys.executable, CHECKER, "--command", command,
            "--package", PACKAGE, "--output", output]
    for case in args.case or []:
        argv.extend(["--case", case])
    try:
        result = subprocess.run(argv, timeout=900)
        report = os.path.join(output, "result.json")
        if os.path.exists(report):
            with open(report, encoding="utf-8") as handle:
                data = json.load(handle)
            failed = [row for row in data.get("cases", [])
                      if row.get("status") != "PASS"]
            if failed:
                for row in failed[:5]:
                    print("FAIL: %s: %s" % (row.get("id"), row.get("message")))
                return 1
            print("PASS: %d public cases of the task package, the protocol "
                  "checker is happy" % len(data.get("cases", [])))
            return 0
        return result.returncode
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())