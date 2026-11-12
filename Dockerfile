# MiniVectorDB - a small vector database with SQL, transactions and crash
# recovery.  The image builds the database from source, the runtime image only
# contains the binary and the C library.
#
#   docker build -t minivectordb:latest .
#   docker run --rm -i -v minivectordb-data:/data minivectordb:latest

FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ make ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/
RUN make -C /src/minivectordb BUILD=/src/minivectordb/build

FROM ubuntu:24.04

WORKDIR /app
COPY --from=builder --chmod=755 /src/minivectordb/build/minivectordb /app/minivectordb
RUN mkdir -p /data && chmod 0777 /data

VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
# The judge speaks the JSON lines protocol: one request object per line, one
# answer object per line, the handshake is {"type":"ready"}.
CMD ["--data-dir", "/data", "--protocol", "jsonl"]