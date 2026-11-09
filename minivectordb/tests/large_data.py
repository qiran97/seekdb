#!/usr/bin/env python3
"""Large data experiment for MiniVectorDB.

The task requires that the database also works when the data set is bigger
than the memory limit.  The script writes a configurable number of rows with
128 dimensional vectors into a database with a small cache budget, then runs
vector top-k queries and reports the peak resident set size.  The rows are
never held in memory as a whole by the database, the queries have to stream
the data through the buffer pool.

Usage:
  python3 tests/large_data.py --binary build/minivectordb [--rows 100000]
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

DIM = 128


def peak_rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid, "r") as handle:
            for line in handle:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def io_counters(pid):
    counters = {}
    try:
        with open("/proc/%d/io" % pid, "r") as handle:
            for line in handle:
                if ":" in line:
                    key, value = line.split(":", 1)
                    counters[key.strip()] = int(value.strip())
    except OSError:
        pass
    return counters


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/minivectordb")
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--batch", type=int, default=500)
    parser.add_argument("--cache-budget", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--buffer-pool", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        print("binary not found: %s" % binary)
        return 2

    workdir = tempfile.mkdtemp(prefix="mvdb-large-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir)
    rng = random.Random(args.seed)
    process = subprocess.Popen(
        [binary, "--data-dir", data_dir, "--log-level", "0",
         "--cache-budget", str(args.cache_budget),
         "--buffer-pool", str(args.buffer_pool)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        process.stdin.write(
            ("CREATE TABLE big (id BIGINT PRIMARY KEY, category VARCHAR(32), "
             "embedding VECTOR(%d));\n" % DIM).encode("utf-8")
        )
        process.stdin.flush()
        process.stdout.readline()

        write_start = time.monotonic()
        io_start = io_counters(process.pid)
        written = 0
        while written < args.rows:
            count = min(args.batch, args.rows - written)
            values = []
            for index in range(count):
                number = written + index + 1
                vector = ",".join("%.5f" % rng.random() for _ in range(DIM))
                values.append(
                    "(%d, 'c%d', '[%s]')" % (number, number % 8, vector)
                )
            process.stdin.write(
                ("INSERT INTO big VALUES\n" + ",\n".join(values) + ";\n").encode("utf-8")
            )
            process.stdin.flush()
            process.stdout.readline()
            written += count
        write_time = time.monotonic() - write_start
        io_write = io_counters(process.pid)

        query_start = time.monotonic()
        for index in range(args.queries):
            target = ",".join("%.5f" % rng.random() for _ in range(DIM))
            process.stdin.write(
                ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM big "
                 "ORDER BY distance LIMIT 10;\n" % target).encode("utf-8")
            )
            process.stdin.flush()
            process.stdout.readline()
            for _ in range(10):
                process.stdout.readline()
            process.stdout.readline()
        query_time = time.monotonic() - query_start
        io_query = io_counters(process.pid)
        peak = peak_rss_kb(process.pid)

        # The data has to survive a restart.
        process.stdin.close()
        process.wait(timeout=120)
        process = subprocess.Popen(
            [binary, "--data-dir", data_dir, "--log-level", "0",
             "--cache-budget", str(args.cache_budget),
             "--buffer-pool", str(args.buffer_pool)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        process.stdin.write(
            ("SELECT id FROM big ORDER BY id DESC LIMIT 1;\n").encode("utf-8")
        )
        process.stdin.flush()
        last = ""
        for _ in range(4):
            line = process.stdout.readline().decode("utf-8").strip()
            if line.isdigit():
                last = line
                break
        process.stdin.close()
        process.wait(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
        shutil.rmtree(workdir, ignore_errors=True)

    data_bytes = (io_write["write_bytes"] - io_start["write_bytes"]) + \
        (io_query["write_bytes"] - io_write["write_bytes"])
    print("rows             : %d" % args.rows)
    print("vector payload   : %.1f MiB (float32, %d dimensions)"
          % (args.rows * DIM * 4 / 1048576.0, DIM))
    print("cache budget     : %.1f MiB" % (args.cache_budget / 1048576.0))
    print("buffer pool      : %.1f MiB" % (args.buffer_pool / 1048576.0))
    print("write time       : %.2f s (%.0f rows/s)"
          % (write_time, args.rows / max(write_time, 1e-9)))
    print("query time       : %.2f s (%.0f queries/s)"
          % (query_time, args.queries / max(query_time, 1e-9)))
    print("data written     : %.1f MiB" % (data_bytes / 1048576.0))
    print("peak rss         : %.1f MiB" % (peak / 1024.0))
    print("row after restart: %s (expected %d)" % (last, args.rows))
    return 0 if last == str(args.rows) else 1


if __name__ == "__main__":
    sys.exit(main())
