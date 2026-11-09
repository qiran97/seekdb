# seekdb - 2026 competition round 2

This branch contains the implementation of **MiniVectorDB**, the lightweight
vector database of the competition task "赛题一：完成一个轻量向量数据库".

| Path | Content |
| --- | --- |
| [`minivectordb/`](minivectordb/) | the database: sources, tests, documentation |
| [`Dockerfile`](Dockerfile) | builds the database from source into a Linux x86_64 image |
| [`agent/conversation.json`](agent/conversation.json) | the agent learning material of this submission |

## Build and run

```bash
make -C minivectordb            # -> minivectordb/build/minivectordb
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data minivectordb:latest
```

The database reads SQL from `stdin` and writes the answers to `stdout`, the
diagnostics go to `stderr`.  The data files are written to `/data`.

## Documentation

* [minivectordb/README.md](minivectordb/README.md) - build, run, supported SQL
* [minivectordb/docs/PROTOCOL.md](minivectordb/docs/PROTOCOL.md) - the
  stdin/stdout protocol
* [minivectordb/docs/DESIGN.md](minivectordb/docs/DESIGN.md) - storage layout,
  transactions, crash recovery and the vector search

## Tests

```bash
make -C minivectordb test
make -C minivectordb bench
```