# Design of MiniVectorDB

## 1. Overview

```
        stdin                                  stdout
          |                                      ^
          v                                      |
   +-------------+   Statement   +-----------+   |  result block
   |  protocol   | ------------> |  parser   |   |
   |  (server)   | <------------ |  (sql)    |   |
   +-------------+   answer      +-----------+
          |                          |
          |                     +----+----+
          |                     | executor|
          |                     +----+----+
          |                          |
          |            +-------------+-------------+
          |            |             |             |
          |      +-----+----+  +-----+-----+  +----+----+
          |      | catalog |  | vector  |  | transactions|
          |      +----+-----+  +----+-----+  +----+-----+
          |           |              |              |
          |      +----+------+-------+------+       |
          |      |  storage engine (pages, WAL)  | <---+
          |      +-------------+-----------------+
```

The code is split into the following translation units:

| File | Content |
| --- | --- |
| `src/base.*` | error codes, the `Status` type, string helpers, utf8 helpers |
| `src/value.*` | vector literals, the float32 validation and the L2 distance kernel |
| `src/sql.*` | lexer, AST and recursive descent parser |
| `src/storage.*` | file access, undo log, buffer pool, page allocation, database header |
| `src/btree.*` | the clustered B+tree |
| `src/condition.*` | resolution and evaluation of the `WHERE` clause |
| `src/engine.*` | catalog, transactions, DDL and DML |
| `src/executor.*` | `SELECT` execution including the vector top-k search |
| `src/server.*` | the stdin/stdout protocol |
| `src/main.cpp` | command line handling and signal handling |

## 2. Files

Everything is written below the data directory:

| File | Content |
| --- | --- |
| `mvdb.dat` | all pages: the database header, the catalog and the table trees |
| `mvdb.undo` | the undo log of the running transaction |

The database uses a single data file so that a commit only has to flush one
file.  The page size is 4 KiB, page 0 is the database header:

```
offset  0  magic "MVDB001"
offset  8  format version
offset 12  page size
offset 16  id of the next free page
offset 24  number of pages
offset 32  head of the free page list
offset 40  first page of the catalog
offset 48  id of the last committed transaction
```

The catalog is a chain of pages that holds the table definitions, the root
page of every table, the row count and the vector index information.  It is
rewritten whenever a table is created, dropped or when the root page of a
table moves.

## 3. Pages and the buffer pool

```
leaf page                        internal page
+-------------+                  +-------------+
| header      | 40 bytes         | header      | 40 bytes
| slot count  |                  | slot count  |
| heap top    |                  | rightmost   |
+-------------+                  +-------------+
| slot 0      | 24 bytes each    | separator 0 | 16 bytes each
| slot 1      |                  | separator 1 |
| ...         |                  | ...         |
+-------------+                  +-------------+
| row data    | grows downwards  |
+-------------+                  |
```

A leaf slot holds the primary key, the offset of the row inside the page and
the length of the row.  A row that does not fit into a page is stored in a
chain of overflow pages, the slot then points to the first page of the chain.

The buffer pool holds 4 KiB frames in a LRU order.  A page is pinned while it
is used, dirty pages are written back when they are evicted.  The size of the
pool is derived from the cgroup memory limit of the container
(`/sys/fs/cgroup/memory.max`), by default the pool uses at most 16 MiB.

## 4. The clustered index

Every table owns one B+tree whose keys are the primary key and whose leaves
store the complete row.  The tree is clustered, a full scan therefore reads
the rows in primary key order and the sort of an ordinary `ORDER BY id` is
free.

* A lookup descends from the root, the separator of an internal page is the
  smallest key of the right hand subtree.
* A leaf that is full is split in the middle, the separator is the first key
  of the new right leaf.  The separator is inserted into the parent, a
  parent that is full is split as well and a new root is created when the
  root itself is split.
* A delete marks the slot as deleted.  The space is reclaimed when the page
  is compacted, the page is never merged with a neighbour.  A deleted key can
  be inserted again, the tombstone slot is reused.

## 5. Transactions, durability and recovery

The database supports one explicit transaction at a time.  A statement that
runs outside of an explicit transaction is executed in its own implicit
transaction and committed immediately.

### 5.1 Write ahead logging

Before a page is modified for the first time inside a transaction, the
*before image* of the whole page is written to the undo log
(`mvdb.undo`).  The log record contains the transaction id, the page id, the
length and a CRC32 checksum.

### 5.2 Commit

```
1. flush the undo log          (the before images reach the file)
2. write all dirty pages       (the new content reaches mvdb.dat)
3. fdatasync(mvdb.dat)         (the data is durable)
4. append the commit record     (the transaction is committed)
5. fdatasync(mvdb.undo)         (the commit record is durable)
6. truncate the undo log
```

The commit record in step 4 and 5 is the commit point.  A statement that
does not change anything (a pure `SELECT`) skips the whole sequence, it does
not force a single disk write.

### 5.3 Rollback

A statement that fails inside a transaction is rolled back to the savepoint
that was taken when the statement started: the before images of the pages
that the statement touched are written back and the undo log is truncated to
the savepoint.  A multi row `INSERT` whose second row violates the primary
key therefore leaves no row behind, which is verified by the test case
`tests/cases/08_statement_atomicity.sql`.  A `ROLLBACK` of the whole transaction uses the position of
the log at the beginning of the transaction.

The in memory mirror of a table (see section 6) is updated in step with the
pages and is restored from a small undo list that the engine keeps per
statement.

### 5.4 Recovery

After a restart the engine reads the undo log:

* if the log contains a commit record, the transaction was committed, the
  data is already on disk and the log is truncated,
* otherwise the before images of every record are written back to the data
  file, which restores the state of the last commit, and the log is
  truncated.

The crash model of the task is a forced termination of the process.  The
undo log is written with `write(2)` and therefore lives in the page cache of
the kernel, which survives the death of the process.  The undo log is reset
after every commit, so at most one transaction can be recovered.

## 6. Memory strategy

The task requires that the database also works with data sets that are
larger than the memory limit.  Two mechanisms keep the memory bounded:

* the buffer pool has a fixed size and evicts pages in LRU order,
* a table is mirrored in memory only when the whole table fits into the
  cache budget.  The budget is derived from the cgroup memory limit (by
  default a quarter of the limit, at most 64 MiB).  A table that is too
  large is read from the clustered index, a scan then streams the pages
  through the buffer pool.

The mirror is filled lazily: the first query after a change rebuilds it if
the table still fits into the budget.  It is used for the point lookups and
for the scans, which keeps the vector search independent of the size of the
table.

## 7. The vector search

`L2_DISTANCE(vector_column, '[...]')` computes the euclidean distance
between the stored float32 vector and the given literal.  The literal is
validated when the statement is prepared: the dimension has to match the
column, the elements have to be finite and they have to fit into a float32.

The kernel accumulates the squared differences in float32 and converts the
result to a double, an AVX2 implementation is selected at runtime when the
processor supports it.

A top-k query is executed in three steps:

1. the `WHERE` clause is evaluated for every row of the table,
2. the distance of every row that passes the filter is computed,
3. a bounded heap of `LIMIT + OFFSET` entries keeps the best candidates.

The heap makes the memory of a top-k query independent of the size of the
table and it stops the scan as soon as the heap is full and the candidate
cannot enter it any more.  Rows with the same distance are ordered by the
primary key, the same rule is used for the ties of an ordinary `ORDER BY`.

The comparison of two candidates is implemented once (`compare_candidates`)
and used by the heap and by the sort of an unlimited result, so that both
paths order the rows in exactly the same way.

The search is exact: the `ann` mode returns the same rows as the `exact`
mode, which keeps the recall at 1.0.  `CREATE VECTOR INDEX` records the
index of the vector column in the catalog, the index is maintained by the
same code path that maintains the rows.

## 8. Performance

The benchmark `tests/bench.py` reproduces the workload of the task (10,000
rows with 128 dimensional vectors, written in batches of 100 rows that are
committed, 1,000 top-k queries with K = 10, half of them with a filter).  On
the development machine (WSL2 on a Windows filesystem, therefore with a very
slow `fsync`) the numbers are:

| Metric | Value |
| --- | --- |
| write phase | 0.60 s for 10,000 rows (16,700 rows/s) |
| write I/O | 23.7 MiB |
| query phase | 1.03 s for 1,000 top-k queries (970 queries/s) |
| query I/O | 0 MiB |
| peak RSS | 21 MiB |

The write phase is dominated by the two `fdatasync` calls per commit, the
query phase runs completely from memory.

## 9. Verification

* `tests/run_tests.py` runs the fixed cases in `tests/cases`.  A case may
  contain `-- !kill` (the server is killed with `SIGKILL` and restarted with
  the same data directory) or `-- !stop` (the server is stopped gracefully
  and restarted), which is how the crash recovery is covered.
* `tests/fuzz.py` is a differential test: it generates a random sequence of
  statements, computes the expected answers with a small reference model in
  python and compares them with the answers of the database.  It found the
  inverted comparison of the top-k heap, a defect that the fixed cases could
  not see because their expected files were recorded from the database
  itself.
* `tests/bench.py` measures the write and the query phase of the
  competition workload.

## 10. Implemented features and known limitations

Implemented:

* `CREATE TABLE` with one `BIGINT PRIMARY KEY`, `VARCHAR(n)` columns and at
  most one `VECTOR(n)` column,
* `INSERT` (single and multi row), `SELECT` (projection, `SELECT *`,
  `L2_DISTANCE`, `WHERE`, `ORDER BY`, `LIMIT`), `UPDATE`, `DELETE`,
* `BEGIN` / `COMMIT` / `ROLLBACK` with one explicit transaction,
* durability across a forced termination of the process,
* `CREATE VECTOR INDEX`, `DROP TABLE`, `DROP VECTOR INDEX`, `SHOW TABLES`,
* the search mode of the session.

Known limitations:

* `NULL`, joins, aggregates, sub queries, `ALTER TABLE`, foreign keys and
  nested transactions are not supported, they are rejected with an error.
* The primary key cannot be updated.
* A `SELECT` without a limit materialises the whole result in memory.
* The vector index is exact, it does not use an approximate candidate
  generation.
* The undo log of a transaction is only truncated after the commit, a
  transaction that touches a very large number of pages therefore uses disk
  space proportional to the number of touched pages.