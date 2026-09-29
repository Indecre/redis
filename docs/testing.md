# Testing

## Layers

The Catch2 v3 test executable is split by intent:

- `tests/unit/resp_test.cpp`: every RESP type, binary content, split-at-every-byte
  fragmentation, pipelines, bad CRLF/lengths, overflow, and limits.
- `tests/unit/engine_test.cpp`: strings and all container types, WRONGTYPE, numeric
  boundaries, negative ranges, sorted-set ties/updates, fake-clock TTL, stale heap
  records, transaction state, pub/sub indexes, graph invariants, and a 2,000-node chain.
- `tests/integration/server_test.cpp`: a real child server on a dynamically reserved
  loopback port; fragmented and pipelined I/O, pub/sub, cross-client WATCH, signal
  shutdown, and full process restart. Polling and child cleanup use bounded deadlines.
- `tests/recovery/aof_test.cpp`: canonical absolute TTLs, graph/TTL replay, interrupted
  final-batch trimming, and fatal corruption.
- `fuzz/resp_parser_fuzz.cpp`: arbitrary bytes fed using changing chunk boundaries under
  bounded parser limits.

## Commands

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug --output-on-failure

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan -j
ctest --preset asan-ubsan --output-on-failure

cmake --preset fuzz
cmake --build --preset fuzz -j
./build/fuzz/keyforge-resp-fuzz -max_total_time=30 -timeout=2
```

Run one discovered Catch2 case with:

```bash
./build/debug/keyforge-tests "RESP parser is incremental at every byte boundary"
```

Enable clang-tidy during a separate configure when it is installed:

```bash
cmake -S . -B build/tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DKEYFORGE_ENABLE_TESTS=ON -DKEYFORGE_ENABLE_CLANG_TIDY=ON
cmake --build build/tidy -j
```

Integration tests require permission to create loopback sockets and fork/exec the local
server. They always terminate their child, escalating from SIGTERM to SIGKILL only after
a one-second deadline. Temporary AOF directories use unique `mkdtemp` paths.

## Manual compatibility smoke test

```bash
./build/release/keyforge-server --appendonly no &
server_pid=$!
redis-cli -p 6380 PING
redis-cli -p 6380 SET smoke value PX 10000
redis-cli -p 6380 GET smoke
kill -TERM "$server_pid"
wait "$server_pid"
```

No timing-sensitive TTL unit test uses real sleep; only process/socket startup polling
uses wall-clock deadlines.
