# MiniVectorDB

MiniVectorDB is a small, self contained vector database.  It implements the
SQL subset, the vector search, the transactions and the crash recovery that
the competition task describes, without using an existing storage engine or
an existing vector index library.

* Storage engine, query executor, vector index and transactions are written
  from scratch, the only dependency is the C++17 standard library.
* The database is a single process that reads SQL from `stdin` and writes the
  results to `stdout`; it never opens a network socket.
* All files live below the data directory (`/data` in the container).

## Build

The build needs nothing but a C++17 compiler:

```bash
make            # -> build/minivectordb
# or
cmake -S . -B build && cmake --build build
```

The `Dockerfile` in the repository root builds the same binary in a
multi stage build:

```bash
docker build -t minivectordb:latest .
```

## Run

```bash
./build/minivectordb --data-dir /data
```

The process stays in the foreground, reads statements from `stdin` and writes
the answers to `stdout`.  Diagnostics go to `stderr` and never mix with the
evaluation output.

```bash
docker run --rm -i -v minivectordb-data:/data minivectordb:latest
```

`docker run` needs `-i` so that the standard input stays connected, the
database does not need a TTY and no port has to be published.

### Command line options

| Option | Meaning |
| --- | --- |
| `--data-dir <path>` | directory that holds `mvdb.dat` and `mvdb.undo` (default `/data`) |
| `--protocol <text\|lines\|jsonl>` | output format of the protocol (default `text`) |
| `--ready-message <text>` | message that is printed once the database is ready |
| `--no-ready-message` | do not print a ready message at all |
| `--result-terminator <text>` | end marker of a result block (default `-- MVDB-END --`) |
| `--no-result-terminator` | do not print a result terminator |
| `--no-header` | do not print the column names of a result set |
| `--mode <exact\|ann>` | default vector search mode (default `exact`) |
| `--buffer-pool <bytes>` | size of the page cache (default: derived from the cgroup limit) |
| `--cache-budget <bytes>` | maximum size of the in memory row cache |
| `--log-level <0..2>` | 0 quiet, 1 info (default), 2 debug |

The environment variables `MINIVECTORDB_DATA_DIR` and
`MINIVECTORDB_SEARCH_MODE` provide the same settings.

## Example session

```sql
CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(4)
);

INSERT INTO docs VALUES
    (1, 'database', 'transaction and recovery', '[1, 0, 0, 0]'),
    (2, 'database', 'vector index',            '[0, 1, 0, 0]'),
    (3, 'travel',   'travel guide',            '[0, 0, 1, 0]');

SELECT id, content,
       L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
FROM docs
WHERE category = 'database'
ORDER BY distance
LIMIT 2;

BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
ROLLBACK;

SELECT id FROM docs ORDER BY id;
```

## Supported SQL

| Statement | Details |
| --- | --- |
| `CREATE TABLE` | `BIGINT PRIMARY KEY`, `VARCHAR(n)`, `VECTOR(n)`, at most one vector column per table |
| `CREATE VECTOR INDEX` | index on the vector column of a table |
| `DROP TABLE`, `DROP VECTOR INDEX` | |
| `INSERT` | single and multi row `VALUES`, optional column list |
| `SELECT` | projection, `SELECT *`, `L2_DISTANCE(...)`, `WHERE`, `ORDER BY`, `LIMIT n [OFFSET m]` |
| `UPDATE`, `DELETE` | with and without `WHERE` |
| `BEGIN`, `COMMIT`, `ROLLBACK` | one explicit transaction at a time |
| `SET search_mode = 'exact' \| 'ann'` | selects the vector search mode of the session |
| `SHOW TABLES` | |

Comparison operators `=`, `<`, `<=`, `>`, `>=` and `<>` are supported in
`WHERE` clauses and combined with `AND`, `OR` and `NOT`.  String comparisons
use the unicode code point order, `VARCHAR` lengths are counted in unicode
characters.

## Protocol

The protocol is described in [docs/PROTOCOL.md](docs/PROTOCOL.md).  In short:
the database prints `MiniVectorDB ready` once the recovery finished, every
statement is terminated with a semicolon and every answer ends with the
result terminator `-- MVDB-END --`.

## Tests

```bash
make test                  # runs the fixed cases and the differential test
python3 tests/run_tests.py --binary build/minivectordb
python3 tests/fuzz.py --binary build/minivectordb --iterations 400 --seed 1
python3 tests/fuzz.py --binary build/minivectordb --mode txn --iterations 300 --seed 1
python3 tests/fuzz.py --binary build/minivectordb --mode recover --iterations 80 --seed 1
make bench                 # runs the performance benchmark
```

`tests/cases` contains mysqltest style cases together with their expected
output.  A case may contain `-- !kill` (the server is killed with `SIGKILL`
and restarted with the same data directory) or `-- !stop` (the server is
stopped gracefully and restarted), which is how the crash recovery is
tested.

`tests/fuzz.py` is a differential test: it generates random statement
sequences and compares the answers of the database with a reference model
that implements the same semantics in python.  `--mode txn` generates random
transaction sequences and checks the visible state after every statement,
`--mode recover` kills the server in the middle of a random write sequence,
restarts it and checks that the committed data survived.

## Documentation

* [docs/PROTOCOL.md](docs/PROTOCOL.md) - the stdin/stdout protocol
* [docs/DESIGN.md](docs/DESIGN.md) - storage layout, transactions, recovery and
  the vector search