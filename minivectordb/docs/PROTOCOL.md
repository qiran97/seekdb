# The MiniVectorDB stdin/stdout protocol

The database is a batch process: it reads SQL statements from `stdin`,
executes them one after the other and writes the answer of every statement to
`stdout`.  Diagnostics are written to `stderr` and never appear on `stdout`.

Everything in this document can be changed with command line options, so a
test driver can adapt the database without rebuilding it.  The defaults are
the ones that are described here.

## 1. Start up

```
$ ./minivectordb --data-dir /data
MiniVectorDB ready
```

The ready message is printed after the crash recovery finished, the database
is only usable afterwards.  `--no-ready-message` suppresses it, a different
text can be configured with `--ready-message`.

## 2. Statement boundaries

* A statement ends with a semicolon `;`.
* A statement may span several lines.
* Semicolons inside a string literal (`'...'`, `"..."`), inside a quoted
  identifier (`` `...` ``) or inside a comment (`-- ...`, `# ...`,
  `/* ... */`) do not end a statement.
* Statements are executed in the order in which they arrive.
* An empty statement (only whitespace and comments) is ignored and produces
  no output.

A driver that sends one statement per line **without** a semicolon is
supported as well: when the input stays idle for a moment (50 milliseconds by
default, `--idle-timeout-ms`) and the pending text is a complete statement,
it is executed.  The parser reports where the statement ends, so a text that
holds several statements is executed statement by statement and a statement
that spans several lines is executed as one statement.  A statement that is
sent in several pieces is not affected, the semicolon still decides where a
statement ends.

## 3. Answers

Every statement produces exactly one answer block.  A block consists of

* a status line,
* an optional result set,
* the result terminator `-- MVDB-END --` (one line, configurable with
  `--result-terminator`, removable with `--no-result-terminator`).

### 3.1 Successful statement

A statement that returns a result set (a `SELECT`) prints the column names
as the first line, followed by one line per row.  Columns and values are
separated by a tabulator.  Inside a value a tabulator, a line feed, a
carriage return and a backslash are escaped as `\t`, `\n`, `\r` and `\\`.

```
id	content	distance
1	transaction	0.14142135923755474
2	vector index	1.2727922061357811
-- MVDB-END --
```

A statement without a result set prints nothing but the terminator:

```
-- MVDB-END --
```

### 3.2 Error

An error prints the error prefix `ERROR`, the numeric error code and the
message, followed by the terminator:

```
ERROR 1062 duplicate primary key value
-- MVDB-END --
```

The error codes follow the MySQL error space where a matching code exists:

| Code | Meaning |
| --- | --- |
| 1030 | the database file could not be read or written |
| 1050 | the table already exists |
| 1054 | unknown column |
| 1062 | duplicate primary key |
| 1064 | syntax error |
| 1146 | unknown table |
| 1235 | the statement is not supported (for example an update of the primary key) |
| 1406 | the value is longer than the column |
| 1416 | the vector literal is invalid (NaN, Infinity, not a number) |
| 1525 | the vector dimension does not match the column |
| 1690 | the value is out of range |
| 1814 | the database file is corrupted |
| 1105 | internal error |

A failed statement does not end an open transaction, the transaction stays
usable and can be committed or rolled back.

### 3.3 Value formats

| Type | Format |
| --- | --- |
| `BIGINT` | decimal number, for example `42` |
| `VARCHAR` | the text as it was stored, escapes as described above |
| `VECTOR` | `[1, 0, 0, 0]`, the float32 values separated by `, ` |
| `L2_DISTANCE` | a decimal number with the shortest representation that round trips to the same double |

## 4. Alternative output formats

`--protocol lines` prints only the rows of a result set, one row per line,
without the column names and without the terminator.  `--protocol jsonl`
prints one JSON object per statement:

```json
{"columns":["id"],"rows":[["1"]],"affected_rows":0}
{"error":{"code":1062,"message":"duplicate primary key value"}}
```

## 5. Session commands

The search mode of the session is switched with

```sql
SET search_mode = 'ann';     -- approximate search
SET search_mode = 'exact';   -- exact search, the default
```

`USE ann`, `USE exact`, `MODE ann` and `MODE exact` are accepted as well.
The mode can also be fixed for the whole run with `--mode ann` or with the
environment variable `MINIVECTORDB_SEARCH_MODE`.

## 6. Shut down

* Closing `stdin` ends the program: an open transaction is rolled back, the
  committed data stays on disk.
* `SIGTERM` and `SIGINT` are handled like a closed `stdin`.
* The program never terminates itself with `exit()` in the middle of a
  statement, the exit code is `0` for a normal shutdown and `1` when the
  database file could not be opened.

## 7. Example of a complete session

```
$ printf "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO t VALUES (1, '[1,0]');\nSELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t;\n" | ./minivectordb --data-dir /tmp/data
MiniVectorDB ready
-- MVDB-END --
-- MVDB-END --
id	d
1	1
-- MVDB-END --
```