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

Two rules keep the pool consistent, both of them were violated before and
cost rows that were silently lost:

* a frame that is given back to the pool must not keep the id of the page it
  held.  The frame is reused for another page later and the stale id would
  remove the mapping of the *new* page, that page would then never be written
  back to the data file,
* a frame that is written back must be reachable in the page table, otherwise
  the write back silently does nothing.

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
* The slots of a leaf page are sorted by their key, the lookups use a binary
  search over the slot array.

The compaction rebuilds the payload area of a page: the payloads of the live
rows are moved to the top of the page and the space of the deleted rows is
reclaimed.  By default the slot array is rebuilt as well, the deleted slots
are removed and the remaining slots move to the front of the array.  That is
not allowed while a scan runs over the page: the scan would see the rows move
to another slot, it would visit some of them twice and skip others.  During a
scan the compaction therefore keeps the layout of the slot array and moves
only the payloads.  For the same reason an `UPDATE` collects the rows that
match its condition first and writes them afterwards, the tree is not changed
while it is scanned.

A leaf that is split only copies the rows that are still alive, a deleted row
stays deleted.  The separator of the split is the first key of the new right
leaf and it is inserted into the parent.  A parent that is full is split
first, the new separator is then placed in the half of the parent that owns
the child and the boundary of the two halves is propagated to the
grandparent.

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
the savepoint.  The state of the tables is part of the savepoint: a rollback
restores the root page and the number of rows of every table, it removes the
tables that the statement created and it restores the tables that it
dropped.  The in-memory mirror of the tables is dropped by a rollback
and rebuilt from the restored pages on the next query, because the mirror can
be built in the middle of a transaction and would not know about the changes
that happened before it existed.  A multi row `INSERT` whose second row violates the primary
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
| write phase | 0.75 s for 10,000 rows (13,300 rows/s) |
| write I/O | 23.7 MiB |
| query phase | 0.65 s for 1,000 top-k queries (1,540 queries/s) |
| query I/O | 0 MiB |
| peak RSS | 21 MiB |

The write phase is dominated by the two `fdatasync` calls per commit, the
query phase runs completely from memory.  The query path was optimised by
measuring the cost of the individual query shapes:

| Query shape (10,000 rows, 128 dimensions) | before | after |
| --- | --- | --- |
| scan + integer key + top-k | 0.57 ms | 0.24 ms |
| scan + filter + integer key + top-k | 0.81 ms | 0.45 ms |
| scan + distance + top-k | 0.60 ms | 0.48 ms |

The scan itself was the dominant cost, not the distance kernel: a query with
a single numeric sort key and a limit is now executed by a specialised loop
that computes the key directly, and the common filter shapes are evaluated
without the generic operand handling.

The write phase is dominated by the two `fdatasync` calls per commit, the
query phase runs completely from memory.

## 9. Verification

* `tests/run_tests.py` runs the fixed cases in `tests/cases`.  A case may
  contain `-- !kill` (the server is killed with `SIGKILL` and restarted with
  the same data directory) or `-- !stop` (the server is stopped gracefully
  and restarted), which is how the crash recovery is covered.
* `tests/fuzz.py` is a differential test: it generates a random sequence of
  statements, computes the expected answers with a small reference model in
  python and compares them with the answers of the database.  In the
  transaction mode the state of every table is compared after every
  statement, a statement that leaves a part of its changes behind is found
  right away.  The sequences write several rows at once, one of them with a
  duplicate key, so that the atomicity of a statement is checked as well.  It found the
  inverted comparison of the top-k heap, a defect that the fixed cases could
  not see because their expected files were recorded from the database
  itself.  The mode `--mode txn` generates random transaction sequences and
  checks the visible state after every statement; it found that a rollback
  restored the pages but not the in-memory mirror of a table.  The mode
  `--mode recover` kills the server in the middle of a random write sequence,
  restarts it and checks that every committed row survived; the interrupted
  statement may be applied completely or not at all.

The differential test found the following defects, all of them are covered by
a fixed case or by the differential test itself now:

| Defect | Effect |
| --- | --- |
| the comparison of the top-k heap was inverted | `ORDER BY distance DESC` returned the worst rows |
| the slots of a leaf page were appended instead of inserted | the binary search of the primary key failed for out of order inserts |
| the compaction moved the payloads in place and in the wrong order | the rows of a page were shifted, a row was read with the payload of another row |
| the compaction dropped the chain id of an overflow row | a long row was lost as soon as its page was compacted |
| a split copied the deleted rows into the new page | deleted rows came back to life |
| the new sibling of a split was not referenced by the parent | the rows of the new leaf could not be found any more |
| the compaction rebuilt the slot array while a scan was running | an `UPDATE` skipped rows and updated some of them twice |
| a frame of the buffer pool kept the id of a page it did not hold | the rows that were written after a `ROLLBACK` were never written to the data file |
| a row that could not be decoded ended the scan silently | the result of a query was truncated without an error |
* The fixed cases cover the order of a top-k query in both directions
  (`cases/18_desc_order.sql`), the rows that are written after a rolled back
  transaction (`cases/19_rollback_data.sql`), rows that are larger than a
  page, they are stored in a chain of overflow pages
  (`cases/20_large_rows.sql`), and the tables that a transaction creates or
  drops (`cases/21_transaction_ddl.sql`).
* `tests/bigtree.py` writes 30,000 rows with random keys, so that the leaf
  pages are split again and again and the tree grows over three levels.  It
  then deletes and updates rows and checks that every row that is still alive
  is found again, with and without the in-memory mirror.
* The same sources are compiled with `-fsanitize=address,undefined` and the
  whole suite is run with that binary.  The sanitizer found the write of a
  row into a string that was too small, the buffer of the row overflowed the
  string object.
* `tests/bench.py` measures the write and the query phase of the
  competition workload.
* `tests/large_data.py` writes a data set that is bigger than the memory
  budget (100,000 rows with 128 dimensional vectors are 48.8 MiB of vector
  payload) into a database with a 16 MiB cache budget and an 8 MiB buffer
  pool.  The peak resident set size stays at 15.0 MiB while the queries
  stream the data through the buffer pool, and the rows survive a restart.
* The differential test is run with 60 seeds in the basic mode and with 30
  seeds in the transaction and in the recovery mode, each with the in memory
  mirror switched on and off, and with the page invariant check enabled.

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