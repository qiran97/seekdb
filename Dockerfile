# MiniVectorDB - a small vector database with SQL, transactions and crash
# recovery.  The image builds the database from source, the runtime image only
# contains the binary.
#
#   docker build -t minivectordb:latest .
#   docker run --rm -i -v minivectordb-data:/data minivectordb:latest
#
# The compiler is only needed while the image is built, the program itself is
# linked statically and the runtime image does not need a library.  The data
# of the database is written to /data.

FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
# The compiler is installed in the build stage, the versions of the packages
# come with the base image.  The update is repeated, a mirror that fails once
# must not fail the build.
RUN set -eux; \
    for attempt in 1 2 3; do \
      if apt-get update; then break; fi; \
      sleep 5; \
    done; \
    apt-get install -y --no-install-recommends g++ make; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/
# A static binary, the runtime image needs no library at all.
RUN make -C /src/minivectordb BUILD=/src/minivectordb/build \
        CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -static"

FROM ubuntu:24.04

WORKDIR /app
COPY --from=builder /src/minivectordb/build/minivectordb /app/minivectordb
RUN chmod 0755 /app/minivectordb && mkdir -p /data && chmod 0777 /data

VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
# The judge speaks the JSON lines protocol: one request object per line, one
# answer object per line, the handshake is {"type":"ready"}.
CMD ["--data-dir", "/data", "--protocol", "jsonl"]
