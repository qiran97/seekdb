# The MiniVectorDB stdin/stdout protocol

The database is a batch process: it reads the requests from `stdin`,
executes them one after the other and writes the answer of every request to
`stdout`.  Diagnostics are written to `stderr` and never appear on `stdout`.

The protocol of the judge is the JSON lines protocol: one JSON object per
line, a request in, an answer out.  It is the default.  The plain text
protocol of the earlier versions is still available with `--protocol text`,
the local test scripts use it.

## 1. Start up

```
$ ./minivectordb --data-dir /data
{"type":"ready"}
```

The ready message is printed after the crash recovery finished, the database
is only usable afterwards.  Every start prints it once, also after a
recovery.  The object must be exactly `{"type":"ready"}`, nothing else may
be written to `stdout` before it.

## 2. Requests

Every request is one JSON object on one line:

```json
{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}
```

| Field | Meaning |
| --- | --- |
| `id` | number of the request, the answer has to carry it back with the same type |
| `op` | `execute` runs the SQL, `shutdown` stops the database |
| `sql` | the statement, it may hold a semicolon |
| `search_mode` | `exact` or `ann`, the mode of the vector search for this request |

The answer of a request is one JSON object on one line:

```json
{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}
{"id":2,"ok":true,"affected_rows":1}
{"id":3,"ok":true}
{"id":4,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
```

* A query answers with `columns` and `rows`, an empty result set has the
  column names and no rows.
* A statement that changes records answers with `affected_rows`.
* A statement that neither queries nor changes records (a data definition
  statement, `BEGIN`, `COMMIT`, `ROLLBACK`) answers with `ok` only.
* A failed statement answers with `ok:false` and an `error` with a non empty
  `code` and a `message`.  The process stays alive and answers the next
  request.

The values of a row are JSON values: a `BIGINT` is an integer, a `VARCHAR` is
a string, a distance is a number and a vector is an array of numbers.

## 3. Normal shutdown

```json
{"id":5,"op":"shutdown"}
```

The database rolls the transaction that is still open back, writes the data
to the disk, answers `{"id":5,"ok":true}` and exits with the code 0.  Nothing
is written to `stdout` after that answer.

## 4. The plain text protocol

With `--protocol text` the database reads the SQL statements from `stdin` and
writes the answers as text.  This is the protocol of the local test scripts.

* A statement ends with a semicolon `;`.
* A statement may span several lines.
* Semicolons inside a string literal (`'...'`, `"..."`), inside a quoted
  identifier (`` `...` ``) or inside a comment (`-- ...`, `# ...`,
  `/* ... */`) do not end a statement.
* Statements are executed in the order in which they arrive.
* An empty statement (only whitespace and comments) is ignored and produces
  no output.

Every statement produces exactly one answer block:

```
id	category
1	alpha
-- MVDB-END --
```

The first line holds the column names, the following lines the rows, the
last line is the terminator `-- MVDB-END --`.  A statement without a result
set answers with the terminator alone, a failed statement answers with
`ERROR <code> <message>` and the terminator.
