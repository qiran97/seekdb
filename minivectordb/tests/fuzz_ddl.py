#!/usr/bin/env python3
"""Randomised test of the data definition statements.

The test creates and drops tables, inside transactions and outside of them,
and checks after every statement that the database shows the tables of the
reference model.  Every few statements the server is restarted: a table that
survives the restart has to keep its rows and a transaction that was open when
the server was stopped has to be rolled back by the recovery.

The catalogue is written every time a table is created or dropped.  It is
spread over several pages as soon as there are many tables, the pages of the
old catalogue are reused and the pages of the chain have to stay consistent.

Usage:
  python3 tests/fuzz_ddl.py [--binary PATH] [--iterations 200] [--seed 1]
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

DIM = 4
VECTOR = "[0.1, 0.2, 0.3, 0.4]"


class Server:
    """The database under test."""

    def __init__(self, binary, data_dir):
        self.binary = binary
        self.data_dir = data_dir
        self.process = None
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [self.binary, "--data-dir", self.data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def stop(self):
        if self.process is None:
            return
        try:
            self.process.stdin.close()
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()
        self.process = None

    def restart(self):
        self.stop()
        self.start()

    def query(self, statement):
        """Runs a statement and returns the lines of the answer."""
        self.process.stdin.write((statement + "\n").encode("utf-8"))
        self.process.stdin.flush()
        lines = []
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("the server closed the connection")
            text = line.decode("utf-8").rstrip("\n")
            if text.startswith("MiniVectorDB ready"):
                continue
            if text == "-- MVDB-END --":
                return lines
            lines.append(text)

    def tables(self):
        answer = self.query("SHOW TABLES;")
        return sorted(line for line in answer if not line.startswith("Tables_in"))

    def rows(self, name):
        answer = self.query("SELECT id FROM %s ORDER BY id;" % name)
        return [line for line in answer if not line.startswith("id")]


def check(server, tables, index, where):
    """Compares the tables of the database with the reference model."""
    got = server.tables()
    want = sorted(tables)
    if got != want:
        return ("statement %d: %s the tables are %r, expected %r"
                % (index, where, got, want))
    for name in want:
        rows = server.rows(name)
        keys = sorted(tables[name])
        if [int(row) for row in rows] != keys:
            return ("statement %d: %s the table %s has the rows %r, expected %r"
                    % (index, where, name, rows, keys))
    return None


def run(binary, rng, iterations, restart_every):
    workdir = tempfile.mkdtemp(prefix="mvdb-ddl-")
    server = Server(binary, workdir)
    # The model of the database: the tables and the keys of their rows.
    tables = {}
    snapshot = None
    counter = 0
    in_transaction = False
    failures = []
    try:
        for index in range(iterations):
            choice = rng.random()
            if not in_transaction and choice < 0.12:
                server.query("BEGIN;")
                in_transaction = True
                snapshot = {name: list(keys) for name, keys in tables.items()}
            elif in_transaction and choice < 0.22:
                server.query("COMMIT;")
                in_transaction = False
                snapshot = None
            elif in_transaction and choice < 0.28:
                server.query("ROLLBACK;")
                in_transaction = False
                # A rollback gives the state of the last commit back.
                tables = snapshot if snapshot is not None else {}
                snapshot = None
            elif choice < 0.68:
                counter += 1
                name = "t%03d" % counter
                server.query("CREATE TABLE %s (id BIGINT PRIMARY KEY, embedding "
                             "VECTOR(%d));" % (name, DIM))
                tables[name] = []
            elif choice < 0.84:
                if not tables:
                    continue
                name = rng.choice(sorted(tables))
                server.query("DROP TABLE %s;" % name)
                del tables[name]
            else:
                if not tables:
                    continue
                name = rng.choice(sorted(tables))
                key = rng.randint(1, 40)
                server.query("INSERT INTO %s VALUES (%d, '%s');" % (name, key, VECTOR))
                if key not in tables[name]:
                    tables[name].append(key)

            failure = check(server, tables, index, "")
            if failure:
                failures.append(failure)
                break

            if restart_every and index % restart_every == restart_every - 1:
                server.restart()
                if in_transaction:
                    # The recovery rolls the open transaction back.
                    in_transaction = False
                    tables = snapshot if snapshot is not None else {}
                    snapshot = None
                failure = check(server, tables, index, "after the restart")
                if failure:
                    failures.append(failure)
                    break
        if in_transaction:
            server.query("COMMIT;")
            in_transaction = False
    finally:
        server.stop()
        shutil.rmtree(workdir, ignore_errors=True)
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--restart-every", type=int, default=25)
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2
    rng = random.Random(args.seed)
    failures = run(binary, rng, args.iterations, args.restart_every)
    if failures:
        for failure in failures[:5]:
            print("FAIL: %s" % failure)
        return 1
    print("PASS: %d statements, the tables of the database agree with the "
          "reference model" % args.iterations)
    return 0


if __name__ == "__main__":
    sys.exit(main())