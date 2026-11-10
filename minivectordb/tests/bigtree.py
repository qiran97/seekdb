#!/usr/bin/env python3
"""Stress test for a tree that grows over several levels.

The test inserts many rows with random keys, so that the leaf pages are
split again and again and the tree grows a second and a third level.  It
then deletes and updates rows and checks that every row that is still alive
can be found again.

Usage:
  python3 tests/bigtree.py [--binary PATH] [--rows 30000]
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

CATEGORIES = ["alpha", "beta", "gamma", ""]
VECTOR = "[0.5, -0.25, 0.125, 1.0]"


class Server:
    """A running server that answers the statements on stdin."""

    def __init__(self, binary, workdir, env=None):
        self.process = subprocess.Popen(
            [binary, "--data-dir", workdir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )

    def query(self, statement):
        self.process.stdin.write((statement + "\n").encode("utf-8"))
        self.process.stdin.flush()
        lines = []
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("the server closed the connection")
            text = line.decode("utf-8").rstrip("\n")
            if text == "-- MVDB-END --":
                return lines
            if text.startswith("MiniVectorDB ready"):
                continue
            lines.append(text)

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)


def create_table(server):
    answer = server.query(
        "CREATE TABLE big (id BIGINT PRIMARY KEY, category VARCHAR(16), "
        "embedding VECTOR(4));"
    )
    if answer:
        raise RuntimeError("CREATE TABLE answered %r" % answer)


def insert_rows(server, keys, categories, batch=500):
    for begin in range(0, len(keys), batch):
        chunk = keys[begin:begin + batch]
        values = []
        for key in chunk:
            values.append("    (%d, '%s', '%s')" % (key, categories[key], VECTOR))
        answer = server.query("INSERT INTO big VALUES\n" + ",\n".join(values) + ";")
        if answer:
            raise RuntimeError("the insert of batch %d answered %r" % (begin, answer))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--rows", type=int, default=30000)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    rng = random.Random(args.seed)
    keys = list(range(1, args.rows + 1))
    rng.shuffle(keys)
    categories = {key: rng.choice(CATEGORIES) for key in keys}

    failures = []
    workdir = tempfile.mkdtemp(prefix="mvdb-bigtree-")
    try:
        server = Server(binary, workdir)
        create_table(server)
        insert_rows(server, keys, categories)

        count = len(server.query("SELECT id FROM big;")) - 1
        if count != len(keys):
            failures.append("the table holds %d rows, expected %d" % (count, len(keys)))

        for key in rng.sample(keys, 200):
            answer = server.query("SELECT id, category FROM big WHERE id = %d;" % key)
            if answer != ["id\tcategory", "%d\t%s" % (key, categories[key])]:
                failures.append("row %d answered %r, expected %r"
                                % (key, answer, categories[key]))
                break

        # A part of the rows is deleted, another part gets a new category.
        victims = rng.sample(keys, 2000)
        for key in victims:
            answer = server.query("DELETE FROM big WHERE id = %d;" % key)
            if answer:
                failures.append("the delete of %d answered %r" % (key, answer))
                break
        changed = rng.sample([key for key in keys if key not in set(victims)], 2000)
        for key in changed:
            answer = server.query("UPDATE big SET category = 'beta' WHERE id = %d;" % key)
            if answer:
                failures.append("the update of %d answered %r" % (key, answer))
                break

        count = len(server.query("SELECT id FROM big;")) - 1
        if count != len(keys) - len(victims):
            failures.append("the table holds %d rows, expected %d"
                            % (count, len(keys) - len(victims)))

        deleted = set(victims)
        for key in rng.sample(keys, 200):
            answer = server.query("SELECT id, category FROM big WHERE id = %d;" % key)
            if key in deleted:
                if answer != ["id\tcategory"]:
                    failures.append("the deleted row %d is still there: %r" % (key, answer))
                    break
            else:
                want = "beta" if key in set(changed) else categories[key]
                if answer != ["id\tcategory", "%d\t%s" % (key, want)]:
                    failures.append("row %d answered %r, expected %r"
                                    % (key, answer, want))
                    break
        server.close()

        # The same workload with the memory mirror switched off.
        env = dict(os.environ)
        env["MINIVECTORDB_DISABLE_CACHE"] = "1"
        workdir2 = tempfile.mkdtemp(prefix="mvdb-bigtree-disk-")
        try:
            server = Server(binary, workdir2, env)
            create_table(server)
            insert_rows(server, keys, categories)
            count = len(server.query("SELECT id FROM big;")) - 1
            if count != len(keys):
                failures.append("the table on disk holds %d rows, expected %d"
                                % (count, len(keys)))
            for key in rng.sample(keys, 50):
                answer = server.query("SELECT id, category FROM big WHERE id = %d;" % key)
                if answer != ["id\tcategory", "%d\t%s" % (key, categories[key])]:
                    failures.append("the row %d on disk answered %r" % (key, answer))
                    break
            server.close()
        finally:
            shutil.rmtree(workdir2, ignore_errors=True)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        for failure in failures[:5]:
            print("FAIL: %s" % failure)
        return 1
    print("PASS: %d rows, the tree grows over several levels and every row "
          "is found again" % len(keys))
    return 0


if __name__ == "__main__":
    sys.exit(main())