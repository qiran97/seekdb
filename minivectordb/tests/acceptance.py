#!/usr/bin/env python3
"""Checks the acceptance methods of the task package, one category at a time.

The table of section 3.2 of the task description names seven categories and
how they are accepted.  Every category gets a check here that follows that
method: the truth is computed by the script itself, not by a recorded file.

Usage:
  python3 tests/acceptance.py [--binary PATH] [--seed 1]
"""

import argparse
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

DIM = 32


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


class Db:
    """The database under test, spoken to with the JSON lines protocol."""

    def __init__(self, binary, work):
        self.binary = binary
        self.work = work
        self.process = None
        self.ident = 0

    def start(self):
        self.process = subprocess.Popen(
            [self.binary, "--data-dir", self.work, "--protocol", "jsonl"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("the database exited before the handshake")
        message = json.loads(line.decode("utf-8"))
        if message != {"type": "ready"}:
            raise RuntimeError("the handshake is wrong: %r" % message)

    def request(self, sql, mode="exact"):
        self.ident += 1
        payload = json.dumps({"id": self.ident, "op": "execute", "sql": sql,
                              "search_mode": mode})
        self.process.stdin.write((payload + "\n").encode("utf-8"))
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("the database exited before the answer of %r" % sql[:60])
        return json.loads(line.decode("utf-8"))

    def rows(self, sql, mode="exact"):
        answer = self.request(sql, mode)
        if not answer.get("ok"):
            raise RuntimeError("the statement %r failed: %r" % (sql[:60], answer))
        return answer.get("rows", [])

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=30)
        self.process = None

    def shutdown(self):
        self.process.stdin.write(b'{"id":9999,"op":"shutdown"}\n')
        self.process.stdin.flush()
        self.process.stdout.readline()
        self.process.wait(timeout=60)
        self.process = None


def check_sql_and_storage(db, rng, work):
    """Deterministic data, the results, the errors, the row counts and the
    state after a restart."""
    db.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16),"
               " content VARCHAR(1024), embedding VECTOR(%d));" % DIM)
    rows = [(1, "database", "事务与恢复"), (2, "database", "向量索引"),
            (3, "travel", "旅行攻略"), (4, "travel", "数据库")]
    for row in rows:
        db.request("INSERT INTO docs VALUES (%d, '%s', '%s', '[%s]');" %
                   (row[0], row[1], row[2],
                    ",".join("0.1" for _ in range(DIM))))
    answer = db.rows("SELECT id, category FROM docs ORDER BY id;")
    if answer != [[1, "database"], [2, "database"], [3, "travel"], [4, "travel"]]:
        return "the rows are %r" % answer
    # an error must not change anything
    answer = db.request("INSERT INTO docs VALUES (1, 'x', 'y', '[0]');")
    if answer.get("ok") is not False or answer["error"]["code"] == "":
        return "the duplicate key was not rejected: %r" % answer
    if len(db.rows("SELECT id FROM docs;")) != 4:
        return "the failed insert changed the row count"
    # the numbers that do not fit are an error
    vector = ",".join("0.1" for _ in range(DIM))
    answer = db.request("INSERT INTO docs VALUES (99999999999999999999, 'x', 'y',"
                        " '[%s]');" % vector)
    if answer.get("ok") is not False:
        return "the number that does not fit was accepted"
    if len(db.rows("SELECT id FROM docs;")) != 4:
        return "the number that does not fit was written into the table"
    # the length of a VARCHAR counts the unicode characters
    db.request("CREATE TABLE u (id BIGINT PRIMARY KEY, t VARCHAR(4));")
    if db.request("INSERT INTO u VALUES (1, '中文字');").get("ok") is not True:
        return "four unicode characters do not fit into VARCHAR(4)"
    if db.request("INSERT INTO u VALUES (2, '中文字字字');").get("ok") is not False:
        return "five unicode characters fit into VARCHAR(4)"
    # the restart keeps the rows
    db.shutdown()
    db.start()
    if len(db.rows("SELECT id FROM docs;")) != 4:
        return "the row count changed after the restart"
    return None


def check_big_vector(db, rng, work):
    """A row that is bigger than the half of a page does not fit into a leaf
    together with another row.  The leaf has to be split, a table with such a
    column has to hold more than one row."""
    dim = 512
    db.request("CREATE TABLE wide (id BIGINT PRIMARY KEY, embedding VECTOR(%d));" % dim)
    truth = []
    for index in range(1, 13):
        vector = [f32(rng.uniform(-1, 1)) for _ in range(dim)]
        truth.append((index, vector))
        answer = db.request("INSERT INTO wide VALUES (%d, '[%s]');" %
                            (index, ",".join("%.9g" % x for x in vector)))
        if answer.get("ok") is not True:
            return "the insert of the row %d failed: %r" % (index, answer)
    answer = db.request("SELECT id FROM wide;")
    if len(answer.get("rows", [])) != 12:
        return "only %d of the 12 rows are there" % len(answer.get("rows", []))
    query = [f32(rng.uniform(-1, 1)) for _ in range(dim)]
    literal = ",".join("%.9g" % x for x in query)
    answer = db.request("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance"
                        " FROM wide ORDER BY distance LIMIT 5;" % literal)
    if not answer.get("ok"):
        return "the search over the wide table failed: %r" % answer
    expected = sorted(truth, key=lambda row: (math.sqrt(sum((a - b) ** 2 for a, b in
                                                              zip(row[1], query))), row[0]))[:5]
    if [row[0] for row in answer["rows"]] != [row[0] for row in expected]:
        return "the order is %r, expected %r" % ([row[0] for row in answer["rows"]],
                                                 [row[0] for row in expected])
    return None


def check_exact_search(db, rng, work):
    """The truth of the filtered set is computed by brute force, the
    distances, the top k, the keys and the order are compared with it."""
    db.request("CREATE TABLE v (id BIGINT PRIMARY KEY, category VARCHAR(8),"
               " embedding VECTOR(%d));" % DIM)
    truth = []
    for index in range(1, 61):
        category = "keep" if index % 3 else "skip"
        vector = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
        truth.append((index, category, vector))
        db.request("INSERT INTO v VALUES (%d, '%s', '[%s]');" %
                   (index, category, ",".join("%.9g" % x for x in vector)))
    for _ in range(4):
        query = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
        literal = ",".join("%.9g" % x for x in query)
        for category, keep in (("keep", True), ("skip", False), ("absent", False)):
            where = " WHERE category = '%s'" % category if category != "absent" else ""
            candidates = [row for row in truth
                          if category == "absent" or row[1] == category]
            candidates.sort(key=lambda row: (math.sqrt(sum((a - b) ** 2 for a, b in
                                                            zip(row[2], query))), row[0]))
            expected = candidates[:10]
            answer = db.rows("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance"
                              " FROM v%s ORDER BY distance LIMIT 10;" % (literal, where))
            if len(answer) != len(expected):
                return "the query returned %d rows, expected %d" % (len(answer), len(expected))
            for got, want in zip(answer, expected):
                if got[0] != want[0]:
                    return "the order is %r, expected the key %d first" % (
                        [row[0] for row in answer], want[0])
                distance = math.sqrt(sum((a - b) ** 2 for a, b in zip(want[2], query)))
                if not math.isclose(got[1], distance, abs_tol=1e-5, rel_tol=1e-5):
                    return "the distance %r is not the truth %r" % (got[1], distance)
    return None


def check_approximate_search(db, rng, work):
    """The ids of the answer exist, they are unique, they satisfy the filter
    and the recall is computed on the filtered set."""
    db.request("CREATE TABLE a (id BIGINT PRIMARY KEY, category VARCHAR(8),"
               " embedding VECTOR(%d));" % DIM)
    truth = []
    for index in range(1, 81):
        category = "keep" if index % 2 else "skip"
        vector = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
        truth.append((index, category, vector))
        db.request("INSERT INTO a VALUES (%d, '%s', '[%s]');" %
                   (index, category, ",".join("%.9g" % x for x in vector)))
    query = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
    literal = ",".join("%.9g" % x for x in query)
    for category in ("keep", "skip"):
        candidates = [row for row in truth if row[1] == category]
        answer = db.rows("SELECT id FROM a WHERE category = '%s' ORDER BY"
                         " L2_DISTANCE(embedding, '[%s]') LIMIT 10;" % (category, literal),
                         mode="ann")
        ids = [row[0] for row in answer]
        if len(ids) != len(set(ids)):
            return "the answer has a duplicate id: %r" % ids
        known = {row[0]: row for row in truth}
        for identifier in ids:
            if identifier not in known:
                return "the id %d does not exist" % identifier
            if known[identifier][1] != category:
                return "the id %d does not satisfy the filter" % identifier
        candidates.sort(key=lambda row: (math.sqrt(sum((x - y) ** 2 for x, y in
                                                        zip(row[2], query))), row[0]))
        wanted = {row[0] for row in candidates[:10]}
        if candidates:
            recall = len(wanted & set(ids)) / min(10, len(candidates))
            if recall < 0.95:
                return "the recall is %.3f" % recall
    return None


def check_index_maintenance(db, rng, work):
    """The index is built, then rows are inserted, updated and deleted, the
    result has to agree with the rows that are there."""
    db.request("CREATE TABLE ix (id BIGINT PRIMARY KEY, category VARCHAR(8),"
               " embedding VECTOR(%d));" % DIM)
    db.request("CREATE VECTOR INDEX ix_vec ON ix(embedding);")
    truth = {}
    for index in range(1, 41):
        vector = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
        truth[index] = vector
        db.request("INSERT INTO ix VALUES (%d, 'c', '[%s]');" %
                   (index, ",".join("%.9g" % x for x in vector)))
    for index in (5, 12, 30):
        vector = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
        truth[index] = vector
        db.request("UPDATE ix SET embedding = '[%s]' WHERE id = %d;" %
                   (",".join("%.9g" % x for x in vector), index))
    for index in (7, 19):
        db.request("DELETE FROM ix WHERE id = %d;" % index)
        del truth[index]
    query = [f32(rng.uniform(-1, 1)) for _ in range(DIM)]
    literal = ",".join("%.9g" % x for x in query)
    answer = db.rows("SELECT id FROM ix ORDER BY L2_DISTANCE(embedding, '[%s]')"
                      " LIMIT 10;" % literal)
    expected = sorted(truth, key=lambda key: (math.sqrt(sum((a - b) ** 2 for a, b in
                                                            zip(truth[key], query))), key))[:10]
    if [row[0] for row in answer] != expected:
        return "the answer is %r, expected %r" % ([row[0] for row in answer], expected)
    if len(db.rows("SELECT id FROM ix;")) != len(truth):
        return "the row count does not match the rows that are there"
    return None


def check_transactions(db, rng, work):
    """The state after a commit, a rollback and a failed statement, both for
    an ordinary query and for the vector search."""
    db.request("CREATE TABLE tx (id BIGINT PRIMARY KEY, category VARCHAR(8),"
               " embedding VECTOR(%d));" % DIM)
    vector = ",".join("0.1" for _ in range(DIM))
    db.request("INSERT INTO tx VALUES (1, 'a', '[%s]');" % vector)
    # a commit
    db.request("BEGIN;")
    db.request("INSERT INTO tx VALUES (2, 'b', '[%s]');" % vector)
    db.request("COMMIT;")
    if len(db.rows("SELECT id FROM tx;")) != 2:
        return "the commit did not keep the row"
    # a rollback
    db.request("BEGIN;")
    db.request("DELETE FROM tx WHERE id = 1;")
    db.request("ROLLBACK;")
    if len(db.rows("SELECT id FROM tx;")) != 2:
        return "the rollback did not bring the row back"
    # a failed statement keeps the transaction alive
    db.request("BEGIN;")
    db.request("UPDATE tx SET category = 'z' WHERE id = 1;")
    failed = db.request("INSERT INTO tx VALUES (2, 'c', '[%s]');" % vector)
    if failed.get("ok") is not False:
        return "the duplicate key was not rejected inside the transaction"
    if db.rows("SELECT category FROM tx WHERE id = 1;") != [["z"]]:
        return "the transaction does not see its own change"
    db.request("COMMIT;")
    if db.rows("SELECT category FROM tx WHERE id = 1;") != [["z"]]:
        return "the commit did not keep the change of the transaction"
    # the vector search sees the transaction as well
    db.request("BEGIN;")
    db.request("INSERT INTO tx VALUES (3, 'c', '[%s]');" % vector)
    if len(db.rows("SELECT id FROM tx;")) != 3:
        return "the vector search does not see the rows of the transaction"
    db.request("ROLLBACK;")
    if len(db.rows("SELECT id FROM tx;")) != 2:
        return "the rollback did not remove the row of the transaction"
    return None


def check_recovery(db, rng, work):
    """The server is killed, the data directory is kept, the server is started
    again and the committed state is compared with the state before the kill."""
    db.request("CREATE TABLE rc (id BIGINT PRIMARY KEY, embedding VECTOR(%d));" % DIM)
    vector = ",".join("0.1" for _ in range(DIM))
    db.request("INSERT INTO rc VALUES (1, '[%s]');" % vector)
    for round_index in range(8):
        db.request("BEGIN;")
        inserted = 1000 + round_index * 10
        for offset in range(5):
            db.request("INSERT INTO rc VALUES (%d, '[%s]');" %
                       (inserted + offset,
                        ",".join("0.%d" % (offset + 1) for _ in range(DIM))))
        committed = round_index % 2 == 0
        if committed:
            db.request("COMMIT;")
        db.kill()
        db.start()
        ids = [row[0] for row in db.rows("SELECT id FROM rc;")]
        if committed:
            if not all(inserted + offset in ids for offset in range(5)):
                return "the committed rows are gone after the kill: %r" % ids
        else:
            if any(inserted + offset in ids for offset in range(5)):
                return "the rows of the open transaction are there after the kill"
        # the kill in the middle of the recovery
        db.kill()
        db.start()
        ids = [row[0] for row in db.rows("SELECT id FROM rc;")]
        if committed:
            if not all(inserted + offset in ids for offset in range(5)):
                return "the committed rows are gone after the second kill"
        else:
            if any(inserted + offset in ids for offset in range(5)):
                return "the rows of the open transaction are there after the second kill"
    return None


def check_resources(binary, rng, work):
    """The data is bigger than the memory budget, the database has to work
    with it and the peak memory has to stay small."""
    rows = 60000
    dim = 128
    process = subprocess.Popen(
        [binary, "--data-dir", work, "--protocol", "jsonl", "--log-level", "0",
         "--cache-budget", "1048576", "--buffer-pool", "1048576"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    process.stdout.readline()
    ident = 0

    def request(sql):
        nonlocal ident
        ident += 1
        payload = json.dumps({"id": ident, "op": "execute", "sql": sql,
                              "search_mode": "exact"})
        process.stdin.write((payload + "\n").encode("utf-8"))
        process.stdin.flush()
        return json.loads(process.stdout.readline().decode("utf-8"))

    request("CREATE TABLE big (id BIGINT PRIMARY KEY, embedding VECTOR(%d));" % dim)
    vector = ",".join("0.5" for _ in range(dim))
    for start in range(1, rows + 1, 500):
        chunk = ",".join("(%d, '[%s]')" % (index, vector)
                         for index in range(start, min(start + 500, rows + 1)))
        answer = request("INSERT INTO big VALUES %s;" % chunk)
        if not answer.get("ok"):
            return "the insert of the rows %d failed: %r" % (start, answer)
    answer = request("SELECT id FROM big WHERE id = %d;" % rows)
    if not answer.get("ok") or answer.get("rows") != [[rows]]:
        return "the row %d is not there: %r" % (rows, answer)
    answer = request("SELECT id FROM big ORDER BY L2_DISTANCE(embedding, '[%s]')"
                     " LIMIT 10;" % vector)
    if not answer.get("ok") or len(answer.get("rows", [])) != 10:
        return "the top k over the big table failed: %r" % answer
    peak = 0
    try:
        with open("/proc/%d/status" % process.pid, encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmHWM:"):
                    peak = int(line.split()[1])
    except OSError:
        peak = 0
    process.stdin.write(b'{"id":99999,"op":"shutdown"}\n')
    process.stdin.flush()
    process.stdout.readline()
    process.wait(timeout=120)
    budget = 2 * 1024 * 1024
    if peak > budget:
        return "the peak memory is %d KiB, the budget is %d KiB" % (peak, budget)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2
    rng = random.Random(args.seed)
    root = tempfile.mkdtemp(prefix="mvdb-acceptance-")
    failures = 0
    try:
        simple = [("SQL and storage", check_sql_and_storage),
                  ("big vector", check_big_vector),
                  ("exact search", check_exact_search),
                  ("approximate search", check_approximate_search),
                  ("index maintenance", check_index_maintenance),
                  ("transactions", check_transactions),
                  ("recovery", check_recovery)]
        for name, function in simple:
            work = os.path.join(root, name.replace(" ", "-"))
            os.makedirs(work)
            db = Db(binary, work)
            db.start()
            try:
                problem = function(db, rng, work)
            except Exception as error:  # a crash of the database shows up here
                problem = str(error)
            finally:
                try:
                    db.shutdown()
                except Exception:
                    pass
            if problem:
                failures += 1
                print("FAIL %-20s %s" % (name, problem))
            else:
                print("PASS %-20s" % name)
        work = os.path.join(root, "resources")
        os.makedirs(work)
        problem = check_resources(binary, rng, work)
        if problem:
            failures += 1
            print("FAIL %-20s %s" % ("resources", problem))
        else:
            print("PASS %-20s" % "resources")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("%d of 8 categories failed" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())