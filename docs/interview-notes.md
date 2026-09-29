# Interview notes

## Truthful 60-second explanation

KeyForge is a C++20, Linux-only, single-node datastore implementing a Redis-compatible
subset. One level-triggered epoll thread owns all sockets and the typed keyspace, so
commands and MULTI/EXEC are atomic without database locks. RESP2 parsing is incremental
and binary-safe, with fragmentation, pipelining, and allocation limits. Five data types,
lazy plus heap-driven expiry, WATCH versions for absent keys, pub/sub, and RESP append-only
recovery are implemented. Its distinctive feature is a bidirectional dependency graph:
cache entries declare parents, cycle checks protect the DAG, and invalidating or expiring
a parent removes the full dependent closure iteratively. The current suite includes real
child-process socket/restart tests and a 2,000-node cascade. It is a learning/portfolio
system, not a production Redis replacement.

## Five-minute walkthrough

1. **Protocol and network boundary.** Accepted fds are non-blocking RAII objects. Epoll
   events carry never-reused connection tokens. Receive bytes enter an owned RESP parser;
   complete argv frames reach the command engine, while partial frames remain buffered.
   Replies are queued, partial sends track an offset, and EPOLLOUT exists only when needed.
2. **Atomic data ownership.** One event thread owns a variant-based keyspace. Sorted sets
   maintain a hash index and an ordered score/member index. Empty containers disappear
   and wrong-type operations never coerce values.
3. **Time and concurrency semantics.** An injectable clock makes expiry exact in tests.
   TTL changes push generation-tagged heap records; stale records are cheap to ignore.
   Key versions survive absence, so WATCH sees creation, deletion, mutation, expiry, and
   graph cascades. EXEC runs its queue before epoll serves another client.
4. **Durability.** Successful mutations are logged as atomic RESP MULTI/EXEC batches.
   Relative TTLs are converted to absolute deadlines. Startup streams the AOF through
   the same parser/dispatcher and trims only an incomplete final batch; middle corruption
   is fatal. Replay defers expiry until graph construction is complete.
5. **Original extension.** Parent-to-child graph edges have reverse indexes for inspection
   and cleanup. Adding an edge checks reachability before any mutation. Cascade BFS uses a
   visited set, handles diamonds, and does not risk stack overflow on deep chains.

## Major tradeoffs

- A single event thread favors understandable atomicity and predictable ownership over
  multicore command throughput. Slow commands such as KEYS or huge cascades still block
  other command execution.
- Level-triggered epoll is simpler and less error-prone here than edge-triggered epoll;
  loops still drain accept/read until EAGAIN.
- Stale expiry heap records trade extra memory for O(log n) TTL updates without arbitrary
  heap deletion.
- AOF batches and dispatcher replay avoid a second state format, but there is no rewrite,
  so disk use grows monotonically.
- `everysec` moves fsync off the normal command path, though a shared descriptor mutex can
  briefly delay an append during the periodic sync.

## Three difficult design problems

### 1. Correct replay when a parent is already expired

Immediately applying a persisted past PEXPIREAT during replay could delete the parent
before later `KFG.DEPEND` records rebuild its edges, turning valid history into a replay
error and leaving derived values. Replay therefore pauses expiry, reconstructs all state
and graph edges, then processes due deadlines and cascades.

### 2. Interrupted compound writes

Logging MSET, SET-plus-TTL, or EXEC as unrelated records could recover a prefix after a
crash. KeyForge wraps every durability unit in RESP MULTI/EXEC, tracks the last completed
outer boundary, leaves incomplete commands queued during replay, and truncates only that
final tail.

### 3. Descriptor reuse and asynchronous delivery

An epoll batch can still contain events for a connection closed earlier in that batch,
and Linux may reuse its fd quickly. Events therefore contain monotonically increasing
connection tokens rather than fd numbers. Pub/sub routes with the same tokens and checks
the live map before queueing each message.

## Reasonable next steps

AOF rewrite with atomic rename, pattern pub/sub, a monotonic runtime expiry mirror,
broader zset ranges, TSan coverage for the fsync worker, richer latency benchmarks, and
fault-injection around short writes/fsync failures are natural extensions. Replication,
cluster mode, and Lua would be separate major projects rather than small checkboxes.
