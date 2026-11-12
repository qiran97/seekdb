#!/usr/bin/env python3
"""Performance benchmark for MiniVectorDB.

The workload follows the description of the competition benchmark:

  * 10,000 records with 128 dimensional float32 vectors,
  * the records are written in batches of 100 rows, every batch is committed,
  * 1,000 vector top-k queries with K = 10, half of them use an equality
    filter on an ordinary column.

The script reports the wall clock time of the write phase and of the query
phase, the amount of data the process read from and wrote to the block
device and the peak resident set size of the process.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

CATEGORIES = ["alpha", "beta", "gamma", "delta", "epsilon"]
DIM = 128
ROWS = 10000
BATCH = 100
QUERIES = 1000
TOPK = 10


def vector_literal(rng):
    values = [rng.random() for _ in range(DIM)]
    return "[" + ",".join("%.6f" % v for v in values) + "]"


def read_proc(pid, name):
    try:
        with open("/proc/%d/%s" % (pid, name), "r") as handle:
            return handle.read()
    except OSError:
        return ""


def io_counters(pid):
    counters = {}
    for line in read_proc(pid, "io").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            counters[key.strip()] = int(value.strip())
    return counters


def peak_rss_kb(pid):
    for line in read_proc(pid, "status").splitlines():
        if line.startswith("VmHWM:"):
            return int(line.split()[1])
    return 0


def read_answer(process):
    """Reads one answer block, it ends with the result terminator."""
    lines = []
    while True:
        line = process.stdout.readline()
        if not line:
            raise RuntimeError("the server closed the connection")
        text = line.decode("utf-8").rstrip("\n")
        if text == "-- MVDB-END --":
            return lines
        if text.startswith("MiniVectorDB ready"):
            continue
        lines.append(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--rows", type=int, default=ROWS)
    parser.add_argument("--queries", type=int, default=QUERIES)
    parser.add_argument("--batch", type=int, default=BATCH)
    parser.add_argument("--dim", type=int, default=DIM)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    workdir = tempfile.mkdtemp(prefix="mvdb-bench-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir)
    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    schema = (
        "CREATE TABLE bench ("
        "id BIGINT PRIMARY KEY,"
        "category VARCHAR(32),"
        "payload VARCHAR(128),"
        "embedding VECTOR(%d));\n" % args.dim
    )
    rows = []
    for index in range(args.rows):
        category = CATEGORIES[index % len(CATEGORIES)]
        rows.append(
            "    (%d, '%s', 'row-%08d', '%s')"
            % (index + 1, category, index, vector_literal(rng))
        )

    queries = []
    for index in range(args.queries):
        target = vector_literal(rng)
        if index % 2 == 0:
            category = CATEGORIES[index % len(CATEGORIES)]
            queries.append(
                "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM bench "
                "WHERE category = '%s' ORDER BY distance LIMIT %d;"
                % (target, category, TOPK)
            )
        else:
            queries.append(
                "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM bench "
                "ORDER BY distance LIMIT %d;" % (target, TOPK)
            )

    process = subprocess.Popen(
        [binary, "--data-dir", data_dir, "--log-level", "0", "--protocol", "text"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    pid = process.pid

    def send(text):
        process.stdin.write(text.encode("utf-8"))
        process.stdin.flush()

    try:
        send(schema)
        read_answer(process)  # the ready message and the answer of the CREATE
        io_start = io_counters(pid)

        write_start = time.monotonic()
        batches = 0
        for begin in range(0, args.rows, args.batch):
            chunk = rows[begin:begin + args.batch]
            send("INSERT INTO bench VALUES\n" + ",\n".join(chunk) + ";\n")
            send("COMMIT;\n")
            read_answer(process)  # the answer of the insert
            read_answer(process)  # the answer of the commit
            batches += 1
            if batches % 10 == 0:
                sys.stderr.write("  batch %d/%d\n" % (batches, (args.rows + args.batch - 1) // args.batch))
                sys.stderr.flush()
        write_time = time.monotonic() - write_start
        io_write = io_counters(pid)

        query_start = time.monotonic()
        for index, statement in enumerate(queries):
            if index % 100 == 0:
                sys.stderr.write("  query %d/%d\n" % (index, len(queries)))
                sys.stderr.flush()
            send(statement + "\n")
            read_answer(process)
        query_time = time.monotonic() - query_start
        io_query = io_counters(pid)
        peak = peak_rss_kb(pid)

        process.stdin.close()
        process.wait(timeout=60)
    finally:
        if process.poll() is None:
            process.kill()
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)

    write_bytes = io_write["write_bytes"] - io_start["write_bytes"]
    write_read = io_write["read_bytes"] - io_start["read_bytes"]
    query_write = io_query["write_bytes"] - io_write["write_bytes"]
    query_read = io_query["read_bytes"] - io_write["read_bytes"]
    print("rows            : %d" % args.rows)
    print("queries         : %d (K=%d)" % (args.queries, TOPK))
    print("write time      : %.3f s" % write_time)
    print("write io        : %.2f MiB (w %.2f / r %.2f)"
          % ((write_bytes + write_read) / 1048576.0, write_bytes / 1048576.0,
             write_read / 1048576.0))
    print("query time      : %.3f s" % query_time)
    print("query io        : %.2f MiB (w %.2f / r %.2f)"
          % ((query_write + query_read) / 1048576.0, query_write / 1048576.0,
             query_read / 1048576.0))
    print("peak rss        : %.2f MiB" % (peak / 1024.0))
    print("throughput      : %.0f rows/s write, %.0f queries/s"
          % (args.rows / max(write_time, 1e-9), args.queries / max(query_time, 1e-9)))
    return 0


if __name__ == "__main__":
    sys.exit(main())