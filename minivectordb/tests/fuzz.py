#!/usr/bin/env python3
"""Randomised differential test for MiniVectorDB.

The script drives the database with random statements and compares every
answer with a small reference model that implements the same semantics in
python.  It is the fastest way to find disagreements between the storage
engine and the intended behaviour of the SQL subset.

Usage:
  python3 tests/fuzz.py --binary build/minivectordb [--iterations 300]
                        [--seed 1] [--verbose]
"""

import argparse
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

CATEGORIES = ["alpha", "beta", "gamma"]
DIM = 4


def vector_literal(rng):
    return "[" + ",".join("%.4f" % rng.uniform(-1, 1) for _ in range(DIM)) + "]"


class Database:
    """Reference model: a list of rows per table."""

    def __init__(self):
        self.tables = {}
        self.transactions = []
        self.committed = None
        self.in_transaction = False

    def begin(self):
        if self.in_transaction:
            return ["ERROR 1568 a transaction is already active"]
        self.committed = {name: list(rows) for name, rows in self.tables.items()}
        self.in_transaction = True
        return []

    def commit(self):
        if not self.in_transaction:
            return []
        self.in_transaction = False
        self.committed = None
        return []

    def rollback(self):
        if not self.in_transaction:
            return []
        self.tables = {name: list(rows) for name, rows in self.committed.items()}
        self.in_transaction = False
        self.committed = None
        return []

    def create(self, name):
        if name in self.tables:
            return ["ERROR 1050 table '%s' already exists" % name]
        self.tables[name] = []
        return []

    def insert(self, name, key, category, vector):
        if self.in_transaction and self.committed is not None:
            pass
        rows = self.tables[name]
        for row in rows:
            if row[0] == key:
                return ["ERROR 1062 duplicate primary key value"]
        rows.append((key, category, vector))
        return []

    def update(self, name, key, category):
        rows = self.tables[name]
        for index, row in enumerate(rows):
            if row[0] == key:
                rows[index] = (row[0], category, row[2])
                return []
        return []

    def delete(self, name, key):
        rows = self.tables[name]
        self.tables[name] = [row for row in rows if row[0] != key]
        return []

    def select(self, name, category_filter, limit):
        rows = self.tables[name]
        if category_filter is not None:
            rows = [row for row in rows if row[1] == category_filter]
        rows = sorted(rows, key=lambda row: row[0])
        if limit is not None:
            rows = rows[:limit]
        return rows


def run_transactions(binary, rng, iterations):
    """Drives random transactions and compares the visible state."""
    model = Database()
    statements = []
    expected = []
    table = "t"
    statements.append(
        "CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(16), "
        "embedding VECTOR(%d));" % (table, DIM)
    )
    expected.append(model.create(table))
    next_key = 1

    for _ in range(iterations):
        choice = rng.random()
        if choice < 0.15:
            statements.append("BEGIN;")
            expected.append(model.begin())
        elif choice < 0.30:
            statements.append("COMMIT;")
            expected.append(model.commit())
        elif choice < 0.42:
            statements.append("ROLLBACK;")
            expected.append(model.rollback())
        elif choice < 0.62:
            key = next_key
            next_key += 1
            category = rng.choice(CATEGORIES)
            vector = [round(rng.uniform(-1, 1), 4) for _ in range(DIM)]
            literal = "[" + ",".join("%.4f" % value for value in vector) + "]"
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');" % (table, key, category, literal)
            )
            expected.append(model.insert(table, key, category, vector))
        elif choice < 0.72:
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            statements.append(
                "UPDATE %s SET category = '%s' WHERE id = %d;" % (table, category, key)
            )
            expected.append(model.update(table, key, category))
        elif choice < 0.80:
            key = rng.randint(1, max(next_key, 2))
            statements.append("DELETE FROM %s WHERE id = %d;" % (table, key))
            expected.append(model.delete(table, key))
        else:
            pass
        # The visible state is checked after every statement.
        statements.append("SELECT id, category FROM %s ORDER BY id;" % table)
        rows = model.select(table, None, None)
        answer = ["id\tcategory"]
        for row in rows:
            answer.append("%d\t%s" % (row[0], row[1]))
        expected.append(answer)

    workdir = tempfile.mkdtemp(prefix="mvdb-txn-")
    try:
        answers = run(binary, statements, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return statements, expected, answers


def run_recover(binary, rng, iterations):
    """Kills the server in the middle of a write sequence and checks the state.

    The crash model of the task allows the interrupted statement to be either
    completely applied or not applied at all, but every committed statement
    has to survive.  The test therefore accepts the two states that bracket
    the interrupted statement and rejects everything else.
    """
    model = Database()
    table = "t"
    statements = []
    states = []  # the visible state after every statement
    statements.append(
        "CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(16), "
        "embedding VECTOR(%d));" % (table, DIM)
    )
    model.create(table)
    states.append(list(model.tables[table]))

    next_key = 1
    for _ in range(iterations):
        choice = rng.random()
        if choice < 0.55:
            key = next_key
            next_key += 1
            category = rng.choice(CATEGORIES)
            vector = [round(rng.uniform(-1, 1), 4) for _ in range(DIM)]
            literal = "[" + ",".join("%.4f" % value for value in vector) + "]"
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');" % (table, key, category, literal)
            )
            model.insert(table, key, category, vector)
        elif choice < 0.75:
            key = rng.randint(1, max(next_key, 2))
            statements.append("DELETE FROM %s WHERE id = %d;" % (table, key))
            model.delete(table, key)
        else:
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            statements.append(
                "UPDATE %s SET category = '%s' WHERE id = %d;" % (table, category, key)
            )
            model.update(table, key, category)
        states.append(list(model.tables[table]))

    workdir = tempfile.mkdtemp(prefix="mvdb-recover-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir)
    try:
        kill_index = rng.randrange(1, len(statements))
        process = subprocess.Popen(
            [binary, "--data-dir", data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        for index, statement in enumerate(statements):
            process.stdin.write((statement + "\n").encode("utf-8"))
            process.stdin.flush()
            if index == kill_index:
                # The server is killed while it is working on the statement.
                time.sleep(0.02)
                process.kill()
                break
            process.stdout.readline()  # the terminator of the answer
        process.wait(timeout=30)

        # The server is restarted with the same data directory.
        process = subprocess.Popen(
            [binary, "--data-dir", data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        process.stdin.write(
            ("SELECT id, category FROM %s ORDER BY id;\n" % table).encode("utf-8")
        )
        process.stdin.flush()
        lines = []
        while True:
            line = process.stdout.readline()
            if not line:
                break
            text = line.decode("utf-8").rstrip("\n")
            if text == "-- MVDB-END --":
                break
            if text == "MiniVectorDB ready" or text == "id\tcategory":
                continue
            lines.append(text)
        process.stdin.close()
        process.wait(timeout=30)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    def normalise(rows):
        return ["%d\t%s" % (row[0], row[1]) for row in sorted(rows)]

    actual = lines
    # states[i] is the state after the statement i, the killed statement may
    # or may not have been applied.
    accepted = []
    for candidate in (states[kill_index - 1], states[kill_index]):
        accepted.append(normalise(candidate))
    if actual in accepted:
        return True, "the state after the crash matches (%d rows)" % len(actual)
    return False, "expected one of %r, got %r" % (accepted, actual)


def run(binary, statements, workdir):
    """Runs the statements and returns the answers of the database."""
    process = subprocess.Popen(
        [binary, "--data-dir", workdir],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    payload = "".join(statement + "\n" for statement in statements)
    out, _ = process.communicate(payload.encode("utf-8"), timeout=120)
    lines = out.decode("utf-8").splitlines()
    # Split the answers: every block ends with the terminator.
    answers = []
    current = []
    for line in lines:
        if line.startswith("MiniVectorDB ready"):
            continue
        if line == "-- MVDB-END --":
            answers.append(current)
            current = []
        else:
            current.append(line)
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--mode", default="basic", choices=["basic", "txn", "recover"])
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    rng = random.Random(args.seed)
    model = Database()
    statements = []
    expected = []
    table = "t"

    statements.append(
        "CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(16), "
        "embedding VECTOR(%d));" % (table, DIM)
    )
    expected.append(model.create(table))

    next_key = 1
    for iteration in range(args.iterations):
        choice = rng.random()
        if choice < 0.40:
            key = next_key
            next_key += 1
            category = rng.choice(CATEGORIES)
            # The model and the database have to see the same values, the
            # literal is rounded to four decimals like the generated text.
            vector = [round(rng.uniform(-1, 1), 4) for _ in range(DIM)]
            literal = "[" + ",".join("%.4f" % value for value in vector) + "]"
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');" % (table, key, category, literal)
            )
            expected.append(model.insert(table, key, category, vector))
        elif choice < 0.50:
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            statements.append(
                "UPDATE %s SET category = '%s' WHERE id = %d;" % (table, category, key)
            )
            expected.append(model.update(table, key, category))
        elif choice < 0.58:
            key = rng.randint(1, max(next_key, 2))
            statements.append("DELETE FROM %s WHERE id = %d;" % (table, key))
            expected.append(model.delete(table, key))
        elif choice < 0.85:
            use_filter = rng.random() < 0.5
            limit = rng.choice([None, 1, 3, 5])
            category = rng.choice(CATEGORIES) if use_filter else None
            if category is None:
                sql = "SELECT id, category FROM %s" % table
            else:
                sql = "SELECT id, category FROM %s WHERE category = '%s'" % (table, category)
            if limit is not None:
                sql += " ORDER BY id LIMIT %d" % limit
            else:
                sql += " ORDER BY id"
            statements.append(sql + ";")
            rows = model.select(table, category, limit)
            answer = ["id\tcategory"]
            for row in rows:
                answer.append("%d\t%s" % (row[0], row[1]))
            expected.append(answer)
        else:
            # A vector query: the reference computes the euclidean distance.
            target = [round(rng.uniform(-1, 1), 6) for _ in range(DIM)]
            use_filter = rng.random() < 0.5
            limit = rng.choice([1, 3])
            category = rng.choice(CATEGORIES) if use_filter else None
            literal = "[" + ",".join("%.6f" % value for value in target) + "]"
            if category is None:
                sql = "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM %s" % (
                    literal,
                    table,
                )
            else:
                sql = (
                    "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM %s "
                    "WHERE category = '%s'" % (literal, table, category)
                )
            sql += " ORDER BY distance LIMIT %d;" % limit
            statements.append(sql)
            rows = model.select(table, category, None)
            scored = []
            for row in rows:
                distance = math.sqrt(
                    sum((a - b) ** 2 for a, b in zip(row[2], target))
                )
                scored.append((distance, row[0]))
            scored.sort()
            answer = ["id\tdistance"]
            for distance, key in scored[:limit]:
                answer.append("%d\t%s" % (key, "%.6f" % distance))
            expected.append(answer)

    if args.mode == "recover":
        ok, detail = run_recover(binary, rng, args.iterations)
        if ok:
            print("PASS: the crash recovery keeps the committed data (%s)" % detail)
        else:
            print("FAIL: %s" % detail)
        return 0 if ok else 1
    if args.mode == "txn":
        statements, expected, answers = run_transactions(binary, rng, args.iterations)
    else:
        workdir = tempfile.mkdtemp(prefix="mvdb-fuzz-")
        try:
            answers = run(binary, statements, workdir)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    failures = 0
    if len(answers) != len(expected):
        print("FAIL: %d answers, %d expected" % (len(answers), len(expected)))
        failures += 1
    for index, (answer, want) in enumerate(zip(answers, expected)):
        if isinstance(want, list) and want and isinstance(want[0], str) \
                and want[0].startswith("ERROR"):
            ok = len(answer) == 1 and answer[0].startswith(want[0])
        elif isinstance(want, list) and want and "\t" in want[0] \
                and "distance" in want[0]:
            ok = len(answer) == len(want)
            if ok and answer:
                ok = answer[0] == want[0]
            if ok:
                for got_line, want_line in zip(answer[1:], want[1:]):
                    got_key, got_value = got_line.split("\t")
                    want_key, want_value = want_line.split("\t")
                    if got_key != want_key:
                        ok = False
                        break
                    got_distance = float(got_value)
                    want_distance = float(want_value)
                    if abs(got_distance - want_distance) > 1e-4 * max(1.0, abs(want_distance)):
                        ok = False
                        break
        else:
            ok = answer == want
        if not ok:
            failures += 1
            print("FAIL statement %d: %s" % (index, statements[index]))
            if args.verbose:
                print("--- statement log ---")
                for number, statement in enumerate(statements[: index + 1]):
                    print("%3d %s" % (number, statement))
            print("  expected: %r" % (want,))
            print("  actual:   %r" % (answer,))
            if failures >= 5:
                break
    if failures == 0:
        print("PASS: %d statements, the database agrees with the reference model"
              % len(statements))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())