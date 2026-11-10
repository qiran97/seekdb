#!/usr/bin/env python3
"""Randomised differential test for MiniVectorDB.

The script drives the database with random statements and compares every
answer with a small reference model that implements the same semantics in
python.  It is the fastest way to find disagreements between the storage
engine and the intended behaviour of the SQL subset.

Modes:
  basic    random inserts, updates, deletes and selects, including column
           lists, multi column updates, string comparisons, ordering by a
           string column, LIMIT/OFFSET and the error paths
  txn      random transaction sequences, the visible state is checked after
           every statement
  recover  a random write sequence that is interrupted with SIGKILL, the
           server is restarted and the committed data has to be intact

Usage:
  python3 tests/fuzz.py [--binary PATH] [--iterations 300] [--seed 1]
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

CATEGORIES = ["alpha", "beta", "gamma", ""]
DIM = 4


def format_float(value):
    """Formats a value the way the database prints a float32."""
    import struct
    single = struct.unpack("f", struct.pack("f", value))[0]
    if single == 0.0:
        return "0"
    return "%.9g" % single


def vector_literal(values):
    return "[" + ", ".join(format_float(value) for value in values) + "]"


def random_vector(rng):
    return [round(rng.uniform(-1, 1), 4) for _ in range(DIM)]


class Database:
    """Reference model of the supported SQL subset.

    A row is the tuple (key, category, vector); the vector of a row that was
    inserted without a value is the zero vector, like in the database.
    """

    def __init__(self):
        self.tables = {}
        self.committed = None
        self.in_transaction = False

    # --- transactions ----------------------------------------------------
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

    # --- schema ----------------------------------------------------------
    def create(self, name):
        if name in self.tables:
            return ["ERROR 1050 table '%s' already exists" % name]
        self.tables[name] = []
        return []

    def drop(self, name):
        if name not in self.tables:
            return ["ERROR 1146 table '%s' does not exist" % name]
        del self.tables[name]
        return []

    def missing(self, name):
        return ["ERROR 1146 table '%s' does not exist" % name]

    # --- data ------------------------------------------------------------
    def insert(self, name, key, category, vector):
        if name not in self.tables:
            return self.missing(name)
        rows = self.tables[name]
        for row in rows:
            if row[0] == key:
                return ["ERROR 1062 duplicate primary key value"]
        rows.append((key, category, list(vector)))
        return []

    def update(self, name, key, category=None, vector=None):
        # A key of None updates every row of the table.
        if name not in self.tables:
            return self.missing(name)
        rows = self.tables[name]
        for index, row in enumerate(rows):
            if key is not None and row[0] != key:
                continue
            new_category = row[1] if category is None else category
            new_vector = row[2] if vector is None else list(vector)
            rows[index] = (row[0], new_category, new_vector)
        return []

    def delete(self, name, key):
        # A key of None deletes every row of the table.
        if name not in self.tables:
            return self.missing(name)
        rows = self.tables[name]
        if key is None:
            self.tables[name] = []
        else:
            self.tables[name] = [row for row in rows if row[0] != key]
        return []

    # --- queries ---------------------------------------------------------
    def filter_rows(self, name, terms):
        rows = self.tables[name]
        for column, op, value in terms:
            rows = [row for row in rows if self.compare(column, row, op, value)]
        return rows

    def compare(self, column, row, op, value):
        if column == "id":
            left = row[0]
        elif column == "category":
            left = row[1]
        else:
            left = row[2]
        if op == "=":
            return left == value
        if op == "<>":
            return left != value
        if op == "<":
            return left < value
        if op == "<=":
            return left <= value
        if op == ">":
            return left > value
        if op == ">=":
            return left >= value
        raise AssertionError("unknown operator " + op)

    def order_rows(self, rows, column, descending):
        # The ties are always broken by the primary key in ascending order, the
        # sort is therefore stable on the primary key first.
        rows = sorted(rows, key=lambda row: row[0])
        if column == "id":
            key = lambda row: row[0]
        elif column == "category":
            key = lambda row: row[1]
        else:
            raise AssertionError("unknown order column " + column)
        return sorted(rows, key=key, reverse=descending)

    def select(self, name, terms, order_column, descending, limit, offset,
               columns):
        if name not in self.tables:
            return self.missing(name)
        rows = self.filter_rows(name, terms)
        if order_column is not None:
            rows = self.order_rows(rows, order_column, descending)
        else:
            # Without an order by the rows are returned in the scan order,
            # which is the order of the primary key.
            rows = sorted(rows, key=lambda row: row[0])
        if offset:
            rows = rows[offset:]
        if limit is not None:
            rows = rows[:limit]
        return rows

    def distance_rows(self, name, terms, target, descending, limit):
        if name not in self.tables:
            return self.missing(name)
        rows = self.filter_rows(name, terms)
        scored = []
        for row in rows:
            distance = math.sqrt(
                sum((a - b) ** 2 for a, b in zip(row[2], target))
            )
            scored.append((distance, row))
        # The ties are broken by the primary key in ascending order.
        scored = sorted(scored, key=lambda item: item[1][0])
        scored = sorted(scored, key=lambda item: item[0], reverse=descending)
        if limit is not None:
            scored = scored[:limit]
        return scored
def format_answer(rows, columns):
    answer = []
    for row in rows:
        line = []
        if columns == ["*"]:
            columns = ["id", "category", "embedding"]
        for column in columns:
            if column == "id":
                line.append(str(row[0]))
            elif column == "category":
                line.append(row[1])
            else:
                line.append(vector_literal(row[2]))
        answer.append("\t".join(line))
    return answer


def generate(rng, iterations):
    """Builds a random statement sequence and the expected answers."""
    model = Database()
    table = "t"
    statements = []
    expected = []
    statements.append(
        "CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(16), "
        "embedding VECTOR(%d));" % (table, DIM)
    )
    expected.append(model.create(table))

    # The statements run on several tables, a table is created and dropped
    # from time to time.
    tables = [table]
    next_keys = {table: 1}
    next_table_id = 1
    for _ in range(iterations):
        choice = rng.random()
        table = rng.choice(tables)
        next_key = next_keys[table]

        if choice < 0.30:
            # Insert with and without an explicit column list.
            key = next_key
            next_key += 1
            category = rng.choice(CATEGORIES)
            vector = random_vector(rng)
            literal = vector_literal(vector)
            style = rng.random()
            if style < 0.6:
                statements.append(
                    "INSERT INTO %s VALUES (%d, '%s', '%s');"
                    % (table, key, category, literal)
                )
            elif style < 0.8:
                statements.append(
                    "INSERT INTO %s (id, category) VALUES (%d, '%s');"
                    % (table, key, category)
                )
                vector = [0.0] * DIM
            else:
                statements.append(
                    "INSERT INTO %s (category, id, embedding) VALUES "
                    "('%s', %d, '%s');" % (table, category, key, literal)
                )
            expected.append(model.insert(table, key, category, vector))
            next_keys[table] = next_key

        elif choice < 0.42:
            # Update of one or two columns, with or without a where clause.
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            use_where = rng.random() < 0.8
            if use_where:
                statements.append(
                    "UPDATE %s SET category = '%s' WHERE id = %d;"
                    % (table, category, key)
                )
                expected.append(model.update(table, key, category=category))
            else:
                statements.append(
                    "UPDATE %s SET category = '%s';" % (table, category)
                )
                expected.append(model.update(table, None, category=category))

        elif choice < 0.50:
            # Update of the vector column.
            key = rng.randint(1, max(next_key, 2))
            vector = random_vector(rng)
            statements.append(
                "UPDATE %s SET embedding = '%s' WHERE id = %d;"
                % (table, vector_literal(vector), key)
            )
            expected.append(model.update(table, key, vector=vector))

        elif choice < 0.58:
            # Delete with and without a where clause.
            if rng.random() < 0.85:
                key = rng.randint(1, max(next_key, 2))
                statements.append("DELETE FROM %s WHERE id = %d;" % (table, key))
                expected.append(model.delete(table, key))
            else:
                statements.append("DELETE FROM %s;" % table)
                expected.append(model.delete(table, None))

        elif choice < 0.68:
            # A new table is created or an existing one is dropped.
            if rng.random() < 0.5 or len(tables) == 1:
                name = "u%d" % next_table_id
                next_table_id += 1
                statements.append(
                    "CREATE TABLE %s (id BIGINT PRIMARY KEY, category "
                    "VARCHAR(16), embedding VECTOR(%d));" % (name, DIM)
                )
                expected.append(model.create(name))
                tables.append(name)
                next_keys[name] = 1
            else:
                name = rng.choice(tables)
                statements.append("DROP TABLE %s;" % name)
                expected.append(model.drop(name))
                tables.remove(name)
                next_keys.pop(name, None)

        elif choice < 0.74:
            # A duplicate key has to be rejected.
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            vector = random_vector(rng)
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');"
                % (table, key, category, vector_literal(vector))
            )
            expected.append(model.insert(table, key, category, vector))

        elif choice < 0.78:
            # An unknown column has to be rejected.
            statements.append(
                "SELECT id FROM %s WHERE no_such_column = 1;" % table
            )
            expected.append(["ERROR 1054 unknown column 'no_such_column'"])

        elif choice < 0.82:
            # A vector literal with the wrong dimension has to be rejected.
            key = next_key
            next_key += 1
            statements.append(
                "INSERT INTO %s VALUES (%d, 'x', '[1, 2, 3]');" % (table, key)
            )
            expected.append(["ERROR 1525 vector dimension mismatch: expected 4, got 3"])

        elif choice < 0.91:
            # A select with a filter, an order and a limit.
            terms = []
            if rng.random() < 0.6:
                column = rng.choice(["id", "category"])
                op = rng.choice(["=", "<>", "<", "<=", ">", ">="])
                if column == "id":
                    value = rng.randint(1, max(next_key, 2))
                    terms.append((column, op, value))
                    sql_term = "%s %s %d" % (column, op, value)
                else:
                    value = rng.choice(CATEGORIES)
                    terms.append((column, op, value))
                    sql_term = "%s %s '%s'" % (column, op, value)
            else:
                sql_term = None
            order_column = rng.choice(["id", "category", None])
            descending = rng.random() < 0.4
            limit = rng.choice([None, 1, 3, 7])
            offset = rng.choice([0, 0, 2]) if limit is not None else 0
            columns = ["id", "category"] if rng.random() < 0.7 else ["*"]
            sql = "SELECT %s FROM %s" % (", ".join(columns), table)
            if sql_term is not None:
                sql += " WHERE " + sql_term
            if order_column is not None:
                sql += " ORDER BY %s%s" % (order_column, " DESC" if descending else "")
            if limit is not None:
                sql += " LIMIT %d" % limit
                if offset:
                    sql += " OFFSET %d" % offset
            statements.append(sql + ";")
            rows = model.select(table, terms, order_column, descending, limit,
                                offset, columns)
            header = ["id", "category"] if columns != ["*"] else ["id", "category", "embedding"]
            answer = ["\t".join(header)]
            answer.extend(format_answer(rows, columns))
            if order_column is None:
                # Without an order by the result is a set.
                expected.append(("unordered", answer))
            else:
                expected.append(answer)

        else:
            # A vector top-k query, with and without a filter.
            target = [round(rng.uniform(-1, 1), 6) for _ in range(DIM)]
            terms = []
            sql_term = None
            if rng.random() < 0.5:
                value = rng.choice(CATEGORIES)
                terms.append(("category", "=", value))
                sql_term = "category = '%s'" % value
            descending = rng.random() < 0.3
            limit = rng.choice([1, 3, 10])
            sql = ("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM %s"
                   % (vector_literal(target), table))
            if sql_term is not None:
                sql += " WHERE " + sql_term
            sql += " ORDER BY distance%s" % (" DESC" if descending else "")
            sql += " LIMIT %d;" % limit
            statements.append(sql)
            scored = model.distance_rows(table, terms, target, descending, limit)
            answer = ["id\tdistance"]
            for distance, row in scored:
                answer.append("%d\t%.6f" % (row[0], distance))
            expected.append(answer)

    return model, statements, expected
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
            vector = random_vector(rng)
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');"
                % (table, key, category, vector_literal(vector))
            )
            expected.append(model.insert(table, key, category, vector))
        elif choice < 0.72:
            key = rng.randint(1, max(next_key, 2))
            category = rng.choice(CATEGORIES)
            statements.append(
                "UPDATE %s SET category = '%s' WHERE id = %d;" % (table, category, key)
            )
            expected.append(model.update(table, key, category=category))
        elif choice < 0.80:
            key = rng.randint(1, max(next_key, 2))
            statements.append("DELETE FROM %s WHERE id = %d;" % (table, key))
            expected.append(model.delete(table, key))
        else:
            statements.append("SELECT id, category FROM %s ORDER BY id;" % table)
            rows = model.select(table, [], "id", False, None, 0, ["id", "category"])
            answer = ["id\tcategory"]
            answer.extend(format_answer(rows, ["id", "category"]))
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
    has to survive.
    """
    model = Database()
    table = "t"
    statements = []
    states = []
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
            vector = random_vector(rng)
            statements.append(
                "INSERT INTO %s VALUES (%d, '%s', '%s');"
                % (table, key, category, vector_literal(vector))
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
            model.update(table, key, category=category)
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
                time.sleep(0.02)
                process.kill()
                break
            process.stdout.readline()
        process.wait(timeout=30)

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

    accepted = []
    for candidate in (states[kill_index - 1], states[kill_index]):
        accepted.append(normalise(candidate))
    if lines in accepted:
        return True, "the state after the crash matches (%d rows)" % len(lines)
    return False, "expected one of %r, got %r" % (accepted, lines)


def compare(expected, answers):
    """Compares the answers with the expected ones, returns the failures."""
    failures = []
    if len(answers) != len(expected):
        failures.append((0, "%d answers, %d expected" % (len(answers), len(expected))))
        return failures
    for index, (answer, want) in enumerate(zip(answers, expected)):
        if isinstance(want, tuple) and want and want[0] == "unordered":
            want = want[1]
            ok = sorted(answer) == sorted(want)
        elif isinstance(want, list) and want and isinstance(want[0], str) \
                and want[0].startswith("ERROR"):
            ok = len(answer) == 1 and answer[0].startswith(want[0])
        elif isinstance(want, list) and want and want[0] == "id\tdistance":
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
            failures.append((index, "expected: %r\n        actual:   %r" % (want, answer)))
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--mode", default="basic",
                        choices=["basic", "txn", "recover"])
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    rng = random.Random(args.seed)

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
        _, statements, expected = generate(rng, args.iterations)
        workdir = tempfile.mkdtemp(prefix="mvdb-fuzz-")
        try:
            answers = run(binary, statements, workdir)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    failures = compare(expected, answers)
    if failures:
        for index, detail in failures[:5]:
            print("FAIL statement %d: %s" % (index, statements[index]))
            print("        %s" % detail)
        print("%d of %d statements disagree with the reference model"
              % (len(failures), len(statements)))
        if args.verbose:
            print("--- statement log ---")
            for number, statement in enumerate(statements):
                print("%3d %s" % (number, statement))
        return 1
    print("PASS: %d statements, the database agrees with the reference model"
          % len(statements))
    return 0


if __name__ == "__main__":
    sys.exit(main())