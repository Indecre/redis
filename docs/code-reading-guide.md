# KeyForge code-reading and improvement guide

This guide is a deliberate path through the actual KeyForge implementation. It is meant
for someone who wants to understand not only what each file does, but also why the pieces
have their current shape, which invariants make the system correct, how to observe those
invariants at runtime, and where an improvement would produce real engineering value.

Do not begin by reading `src/commands/engine.cpp` from top to bottom. It is the largest
file in the project and makes much more sense after the protocol, storage, and server
boundaries are familiar.

## The mental model to keep in your head

```text
client bytes
    |
    v
non-blocking socket -> per-client RESP parser -> argv vector
                                             |
                                             v
                                    command engine + session
                                      /       |       \
                                     v        v        v
                              typed database  pub/sub  AOF sink
                                /       \
                               v         v
                         expiry heap   dependency graph
                                     parent -> child
```

One event-loop thread owns all mutable database, transaction, graph, and pub/sub state.
That fact is the foundation of command atomicity. The AOF `everysec` worker is the only
background thread, and it never owns or reads the keyspace.

## Set up a reading build

Use Debug so names, assertions from dependencies, and debugger behavior remain useful:

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug --output-on-failure
```

If Ninja is unavailable, the equivalent fallback is:

```bash
cmake -S . -B build/reading -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DKEYFORGE_ENABLE_TESTS=ON
cmake --build build/reading -j
ctest --test-dir build/reading --output-on-failure
```

Keep two terminals open while reading. Run the server in one:

```bash
./build/debug/keyforge-server --port 6380 --appendonly no
```

Use the CLI or `redis-cli` in the other:

```bash
./build/debug/keyforge-cli -p 6380 PING
./build/debug/keyforge-cli -p 6380 SET product:42 data PX 60000
./build/debug/keyforge-cli -p 6380 GET product:42
```

Replace `build/debug` with your chosen build directory when using the fallback.

## Repository map

| Area | Start with | Implementation | What it owns |
|---|---|---|---|
| Build | `CMakeLists.txt` | `cmake/ProjectOptions.cmake` | Targets, warnings, sanitizers, tests, fuzzing |
| Process entry | `src/main.cpp` | `src/server/config.cpp` | Argument validation and server construction |
| Ownership primitives | `include/keyforge/common/fd.hpp` | `include/keyforge/common/clock.hpp` | Descriptor RAII and injected time |
| Protocol | `include/keyforge/protocol/resp.hpp` | `src/protocol/resp.cpp` | RESP values, incremental parsing, encoding, limits |
| Networking | `include/keyforge/server/server.hpp` | `src/server/server.cpp` | Listener, epoll, clients, partial I/O, shutdown |
| Commands | `include/keyforge/commands/engine.hpp` | `src/commands/engine.cpp` | Registry, validation, transactions, command semantics |
| Keyspace | `include/keyforge/storage/database.hpp` | `src/storage/database.cpp` | Typed entries, versions, expiry, graph ownership |
| Sorted sets | `include/keyforge/storage/sorted_set.hpp` | `src/storage/sorted_set.cpp` | Member lookup and score/member ordering |
| Graph | `include/keyforge/graph/dependency_graph.hpp` | `src/graph/dependency_graph.cpp` | Parent/dependent indexes, cycle checks, cascade traversal |
| Persistence | `include/keyforge/persistence/aof.hpp` | `src/persistence/aof.cpp` | Atomic records, fsync modes, replay and tail repair |
| CLI | `apps/cli/main.cpp` | same file | Blocking client, argv encoding, response display |
| Tests | `tests/unit/resp_test.cpp` | other files under `tests/` | Executable specifications of invariants |
| Fuzzing | `fuzz/resp_parser_fuzz.cpp` | same file | Arbitrary parser inputs and chunk boundaries |

The public headers are intentionally small. Read each header before its `.cpp`: the
header tells you the ownership and API contract; the implementation tells you how the
contract is maintained.

## Recommended reading order

### Pass 1: build and process entry

Read these files in order:

1. `CMakeLists.txt`
2. `cmake/ProjectOptions.cmake`
3. `CMakePresets.json`
4. `src/main.cpp`
5. `include/keyforge/server/config.hpp`
6. `src/server/config.cpp`

Questions to answer:

- Which source files belong to `keyforge_core`, and why are `main.cpp` and the CLI kept
  outside the reusable library?
- Which warnings apply only to project code?
- Why is Catch2 fetched only when tests are enabled?
- Why does the Release preset turn tests off while Debug keeps them on?
- At what point can invalid configuration fail relative to opening a listener?

Follow `main` precisely. It parses arguments, handles help/version without touching the
network, constructs `Server`, calls `initialize`, and only then enters `run`. This thin
entry point is the first example of the project's ownership style.

Small experiment:

```bash
./build/debug/keyforge-server --port 0
./build/debug/keyforge-server --max-bulk-bytes 100 --max-client-buffer-bytes 50
./build/debug/keyforge-server --help
```

Find the exact validation branch that produces each result.

### Pass 2: RAII and time

Read:

- `include/keyforge/common/fd.hpp`
- `include/keyforge/common/clock.hpp`
- `tests/unit/engine_test.cpp`, focusing only on `FakeClock`

`Fd` is move-only. Its destructor closes a valid descriptor, move construction uses
`std::exchange`, and `reset` closes the previous resource before accepting another. Trace
every place `Fd` appears in the server and AOF; there should be no corresponding manual
close on an owning success path.

`Clock` exposes two notions of time:

- Unix milliseconds are stable across restart and are written to AOF.
- Monotonic milliseconds schedule runtime expiry and calculate remaining TTL.

The distinction prevents a wall-clock adjustment from changing an already-running TTL.
The test named `runtime TTL uses monotonic time across a wall-clock jump` is the shortest
executable explanation.

Invariant checklist:

- An owning descriptor exists in exactly one `Fd`.
- Moving an `Fd` invalidates the source.
- Runtime deadlines never depend on a later wall-clock reading.
- Persisted deadlines remain meaningful after process restart.

### Pass 3: RESP data model and encoder

Read `include/keyforge/protocol/resp.hpp` before `src/protocol/resp.cpp`.

Start with `Type` and `Value`. `Value` deliberately uses one struct rather than a recursive
`std::variant`: only the fields relevant to the selected `type` are interpreted. Factory
methods make construction explicit. `std::string` is byte storage, not assumed text.

Next read `append_encoded`, `encode`, and `encode_command`. Confirm these rules manually:

- A bulk length counts bytes, including embedded NUL bytes.
- Null bulk strings and null arrays are different values.
- Commands are arrays of bulk strings; one CLI argv item becomes exactly one bulk value.
- Array encoding recursively encodes each child.

Run just the encoder test:

```bash
./build/debug/keyforge-tests "RESP encoder supports every required value type"
```

Then change the test locally to encode a value containing `\r\n` and `\0`. Observe why
bulk framing remains unambiguous. Revert the experimental test afterward.

### Pass 4: incremental parser

Read these internal functions in order:

1. `read_line`
2. `parse_integer`
3. `parse_at`
4. `Parser::feed`
5. `Parser::next`

`parse_at` returns an internal status plus the next byte offset. It does not mutate the
owned buffer. `Parser::next` erases only after a complete top-level frame. That separation
is what makes incomplete input safe.

For each RESP marker (`+`, `-`, `:`, `$`, `*`), write down:

- what header is expected;
- which negative value is legal;
- when the parser returns incomplete;
- which limit is checked before allocation;
- how the exact consumed offset is obtained.

`consumed_bytes` is not cosmetic: AOF replay uses it to identify safe truncation
boundaries. A protocol feature therefore supports a persistence invariant later.

Run:

```bash
./build/debug/keyforge-tests "RESP parser is incremental at every byte boundary"
./build/debug/keyforge-tests "RESP parser rejects malformed input and numeric overflow"
./build/debug/keyforge-tests "RESP parser enforces configured limits"
```

Useful exercise: draw the buffer after feeding half of `$5\r\nhello\r\n`, then after the
rest is fed, then after `next`. Repeat with two pipelined frames and verify why the second
one survives the first call.

### Pass 5: server initialization and epoll ownership

Read `include/keyforge/server/server.hpp`, then the beginning of
`src/server/server.cpp` through `Server::Impl::initialize`.

The public class is a Pimpl wrapper. The private implementation owns configuration,
clock, command engine, optional AOF, listener, epoll descriptor, signal descriptor, and
all clients.

Initialization order matters:

1. Block SIGINT/SIGTERM and create `signalfd`.
2. Open and replay AOF before accepting traffic.
3. Create the listener and set non-blocking/close-on-exec flags with `fcntl`.
4. Apply `SO_REUSEADDR`, bind, and listen.
5. Create epoll and register listener/signal tokens.

Why replay before bind? A corrupt database must prevent the server from advertising a
port that cannot serve valid state.

Notice the special epoll tokens `1` and `2`, followed by monotonically increasing client
tokens. The event payload does not store a raw client fd. This prevents an old epoll event
from acting on an unrelated connection after Linux reuses a descriptor number.

### Pass 6: event loop and connection state

Continue through these methods:

1. `Server::Impl::run`
2. the nested `Client` struct
3. `accept_ready`
4. `client_ready`
5. `read_client`
6. `process_frames`
7. `queue_reply`
8. `write_client`
9. `remove_client`

Trace a PING using this exact path:

```text
epoll_wait
  -> client_ready(EPOLLIN)
  -> read_client(recv until EAGAIN)
  -> Parser::feed / Parser::next
  -> command_arguments
  -> Engine::execute
  -> queue_reply
  -> write_client(send until complete or EAGAIN)
  -> update_interest
```

Important networking invariants:

- Accept and receive loops continue until `EAGAIN`.
- `EINTR` retries rather than becoming a client failure.
- A parser frame is unrelated to `recv` call boundaries.
- `EPOLLOUT` is present only while output remains.
- `output_offset` handles a partial `send` without duplicating bytes.
- `MSG_NOSIGNAL` prevents a closed peer from terminating the process.
- Oversized pending output disconnects the slow consumer.
- Disconnect calls `Engine::disconnect`, which cleans transaction and pub/sub state.

Observe syscalls on a disposable instance:

```bash
strace -ff -e trace=network,epoll_wait,epoll_ctl,fcntl,read,write,fdatasync \
  ./build/debug/keyforge-server --port 6381 --appendonly no
```

In another terminal, send PING and QUIT. Match the syscall trace to the methods above.

### Pass 7: storage type system

Read:

1. `include/keyforge/storage/database.hpp`
2. `src/storage/database.cpp`, initially ignoring expiration methods
3. `include/keyforge/storage/sorted_set.hpp`
4. `src/storage/sorted_set.cpp`

`Data` is a variant of string, list, hash, set, and sorted set. An `Entry` adds expiration
metadata. `Database` separately owns versions, the expiry heap, and the dependency graph.

Understand the behavioral differences between these methods:

- `find`: performs lazy expiry, then returns a non-owning pointer.
- `create`: inserts a new typed value and bumps the key version.
- `replace`: replaces type/value, clears TTL, and bumps the version.
- `touch`: records an in-place mutation.
- `erase`: deletes one value and cleans incident graph edges.
- `invalidate`: computes and removes a transitive graph closure.

The version map outlives entries. That is why WATCH can observe an absent key becoming
present and an existing key becoming absent.

For `SortedSet`, keep both indexes visible in your notes:

```text
unordered_map<member, score>       fast member lookup
set<pair<score, member>>           ordered traversal
```

Read `SortedSet::add` carefully. Updating a score must erase the old ordered pair before
inserting the new one. Equal scores fall back to lexicographic member ordering through
`ScoreMemberLess`.

Suggested invariant test to write yourself:

1. Add members `b` and `a` at score 1.
2. Move `a` to score 2.
3. Remove `b`.
4. After each step, assert lookup and range agree.

### Pass 8: expiry and versioning

Return to these database methods:

- `set_expiration`
- `persist`
- `expiration`
- `remaining_ttl`
- `active_expire`
- `expire_if_needed`
- `bump_version`

One entry stores both its persisted Unix deadline and scheduled monotonic deadline. Each
TTL update receives a unique token and pushes a new heap record. Old heap records are not
deleted eagerly.

When a heap record is popped, all of these must still match:

- the key exists;
- the entry still has expiry metadata;
- the token equals the entry's current token;
- the current monotonic time reached the deadline.

If any check fails, the record is stale and ignored. The event loop supplies a work limit
of 64 records per iteration, preventing an expiration wave or stale-record backlog from
monopolizing one turn. Lazy expiry remains exact when a client touches a key.

Run:

```bash
./build/debug/keyforge-tests \
  "expiration is deterministic at the exact boundary and ignores stale heap records"
./build/debug/keyforge-tests \
  "runtime TTL uses monotonic time across a wall-clock jump"
```

### Pass 9: dependency graph

Read `dependency_graph.hpp` and `dependency_graph.cpp` completely; this is one of the
smallest self-contained modules.

The indexes mean:

```text
parents_[child]       = immediate inputs of child
dependents_[parent]   = immediate values made stale by parent
```

For `KFG.DEPEND child parent`, invalidation flows `parent -> child`.

Read the algorithms in this order:

1. `reachable`: iterative DFS used for cycle detection.
2. `add_dependencies`: validates every proposed parent before mutating anything.
3. `cascade`: iterative BFS with a visited set and sorted immediate dependents.
4. `remove_key`: removes both sides of every incident edge.

Prove the cycle test to yourself. Adding `parent -> child` is illegal exactly when
`parent` is already reachable by following dependent edges from `child`.

Then inspect how `Database::invalidate` first captures the closure, deletes values and
bumps versions, and only afterward removes graph nodes. Removing edges before discovering
the whole closure would silently lose descendants.

Run the diamond, deletion, and deep-chain tests:

```bash
./build/debug/keyforge-tests "dependency graph rejects cycles and invalidates diamonds and expiry cascades"
./build/debug/keyforge-tests "dependency graph handles duplicates deletion transactions and WATCH cascades"
./build/debug/keyforge-tests "dependency graph handles a deep iterative cascade"
```

### Pass 10: command engine and transactions

Now read `include/keyforge/commands/engine.hpp`. Understand `Session`, `Delivery`, and
`Result` before opening the implementation.

Read `src/commands/engine.cpp` in sections rather than linearly:

1. `CommandId`, `CommandSpec`, and `command_specs`
2. parsing/validation helpers such as `parse_i64`, `parse_double`, and `validate_syntax`
3. `canonical_commands`
4. `Engine::execute`, `execute_replay`, and `execute_internal`
5. the transaction-control portion of `execute_internal`
6. `execute_direct`, one command family per sitting
7. pub/sub code near the end

There are three validation times:

- registry lookup and arity validation;
- queue-time syntax validation inside MULTI;
- runtime type/state validation when a handler executes.

A queue-time error marks the transaction failed. EXEC then returns `EXECABORT`. A runtime
WRONGTYPE error becomes one array element and does not undo earlier commands.

WATCH records `Database::version` values in the connection's `Session`. EXEC calls
`version` again; that call can trigger lazy expiry, so an expired watched key conflicts
even when no client explicitly deleted it.

Trace a two-client conflict:

```text
client A: WATCH shared      stores version N
client A: MULTI
client A: SET result stale  queues argv
client B: SET shared new    bumps shared to N+1
client A: EXEC              compares versions, returns null array
```

Run the direct and real-socket versions:

```bash
./build/debug/keyforge-tests "WATCH detects absent-key mutation and expiry while successful EXEC is atomic"
./build/debug/keyforge-tests "actual clients observe WATCH conflicts"
```

When reading each command family, make a small table containing missing-key reply,
wrong-type reply, whether TTL is preserved, when the version changes, and whether an
empty container is deleted. That table is more valuable than memorizing individual `if`
statements.

### Pass 11: persistence

Read:

1. `include/keyforge/persistence/aof.hpp`
2. `canonical_commands` in the engine again
3. `Aof::append_batch`
4. `Aof::replay`
5. `worker_loop`, `flush`, and `stop_worker`
6. the AOF setup block inside `Server::Impl::initialize`

`Sink` is intentionally tiny so tests can replace disk persistence with `MemorySink`.
The engine owns no AOF object; it holds a non-owning sink pointer configured by the
server.

Trace `SET key value PX 5000`:

```text
execute SET in memory
  -> canonical_commands reads final state
  -> [SET key value, PEXPIREAT key absolute-unix-ms]
  -> Aof::append_batch adds outer MULTI and EXEC
  -> write all bytes
  -> maybe fdatasync depending on policy
  -> return reply
```

The AOF outer transaction is recovery framing, not the client's transaction state. If a
write stops halfway, replay may parse MULTI and some queued commands but cannot apply them
without EXEC.

During replay:

1. expiration is paused;
2. RESP commands pass through `Engine::execute_replay`;
3. new AOF writes and live publication are suppressed;
4. completed outer EXEC offsets become safe boundaries;
5. an incomplete final batch is truncated to the last boundary;
6. expiration resumes, and already-expired parents cascade through the reconstructed
   graph.

Pausing expiry is subtle but necessary. Otherwise an expired parent could vanish before
a later AOF record restores the child's dependency edge.

Run:

```bash
./build/debug/keyforge-tests "AOF canonicalizes relative TTL as an absolute deadline"
./build/debug/keyforge-tests "AOF restart restores values TTL and dependency relationships"
./build/debug/keyforge-tests "AOF trims an interrupted final record but rejects earlier corruption"
```

Inspect a disposable AOF:

```bash
mkdir -p /tmp/keyforge-reading
./build/debug/keyforge-server --port 6382 --data-dir /tmp/keyforge-reading \
  --appendonly yes --appendfsync always
# send a few commands, then stop with Ctrl+C
xxd /tmp/keyforge-reading/appendonly.aof | less
```

### Pass 12: pub/sub

The two indexes live in `Engine`:

```text
channel_subscribers_[channel]       -> connection tokens
connection_channels_[connection]   -> channel names
```

Read the subscribed-mode check in `execute_internal`, then PUBLISH, SUBSCRIBE,
UNSUBSCRIBE in `execute_direct`, and finally `Engine::disconnect`.

PUBLISH does not write sockets. It returns `Delivery` objects to the server. The server
queues each delivery through the same output limit used for normal replies and enables
EPOLLOUT for affected recipients. A slow subscriber is therefore isolated from the
publisher and eventually disconnected by normal backpressure.

Verify that PUBLISH is not marked mutating in `command_specs`; that is why ephemeral
messages never enter AOF.

### Pass 13: CLI and end-to-end behavior

Read `apps/cli/main.cpp` last among runtime components. It is deliberately blocking
because it is a client utility, not part of the event-driven server.

Follow:

```text
parse_options -> getaddrinfo/connect -> encode_command -> send_all
              -> receive_one/incremental parser -> print_value
```

One-shot mode preserves argv boundaries. Interactive mode uses `istringstream` and
therefore cannot preserve quoting; this is documented rather than hidden.

Try:

```bash
./build/debug/keyforge-cli -p 6380 SET greeting "hello world"
./build/debug/keyforge-cli -p 6380 GET greeting
```

Then compare with interactive input `SET greeting hello world` and explain the arity
error from the tokenizer.

### Pass 14: tests as executable specifications

Read tests in this order:

1. `tests/unit/resp_test.cpp`
2. `tests/unit/engine_test.cpp`
3. `tests/recovery/aof_test.cpp`
4. `tests/integration/server_test.cpp`
5. `fuzz/resp_parser_fuzz.cpp`

The integration support types are worth studying:

- `ServerProcess` guarantees child cleanup after assertion failures.
- Port reservation avoids hard-coded test ports.
- `Client` retains a parser across reads and uses bounded polling deadlines.
- `TempDirectory` isolates recovery state.

The fuzz harness derives changing chunk sizes from the input itself. Thus the same byte
sequence explores both malformed content and different network fragmentation shapes.

## Four end-to-end traces to reproduce on paper

If you can explain these without looking at the code, you understand most of KeyForge.

### Trace A: fragmented PING

1. Client sends a RESP command one byte at a time.
2. Each epoll wakeup drains currently available bytes.
3. `Parser::next` reports incomplete without consuming a prefix.
4. The final CRLF completes an argv array.
5. Registry validation selects PING.
6. The reply encoder emits `+PONG\r\n`.
7. Output is sent immediately or retained behind EPOLLOUT.

### Trace B: SET with expiration and restart

1. SET validates NX/XX and EX/PX syntax.
2. Database replacement clears an old TTL and bumps the key version.
3. Expiry stores Unix and monotonic deadlines plus a new token.
4. AOF logs SET plus absolute PEXPIREAT inside a batch.
5. Restart replays the absolute deadline while expiry is paused.
6. Replay converts remaining Unix duration to a new monotonic deadline.
7. Expiry resumes; the key is retained or removed based on real remaining time.

### Trace C: parent expiry cascade with WATCH

1. Child and parent exist; graph stores `parent -> child`.
2. A client watches `child`.
3. Parent reaches its monotonic deadline.
4. Active or lazy expiry calls `Database::invalidate(parent)`.
5. BFS closure contains parent and child once each.
6. Both values are erased and both versions advance.
7. The watching client's EXEC compares child versions and aborts.

### Trace D: pub/sub backpressure

1. SUBSCRIBE updates both indexes and returns an acknowledgement array.
2. PUBLISH snapshots recipient tokens and returns `Delivery` values.
3. Server appends each encoded message to the recipient output buffer.
4. Publisher receives the recipient count without waiting for subscriber socket writes.
5. EPOLLOUT drains partial sends later.
6. A recipient exceeding its configured pending-output cap is disconnected and removed
   from both indexes.

## Debugging tools and useful breakpoints

Build Debug, then start GDB:

```bash
gdb --args ./build/debug/keyforge-server --port 6383 --appendonly no
```

Useful breakpoints:

```gdb
break keyforge::commands::Engine::execute_internal
break keyforge::commands::Engine::execute_direct
break keyforge::storage::Database::set_expiration
break keyforge::storage::Database::invalidate
break keyforge::persistence::Aof::append_batch
run
```

At an engine breakpoint:

```gdb
print arguments
print session.in_transaction
bt
next
```

Other useful commands:

```bash
# Find definitions and call sites
rg -n "active_expire|invalidate|append_batch|execute_internal" include src tests

# See target and link commands
cmake --build build/debug --verbose

# Inspect open descriptors of a running server
ls -l /proc/$(pgrep -n keyforge-server)/fd

# Run sanitizers
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan -j
ctest --preset asan-ubsan --output-on-failure
```

Generate a tags database if Universal Ctags is installed:

```bash
ctags -R include src apps tests
```

## How to make changes safely

Use this loop for every improvement:

1. State the invariant or user-visible behavior being changed.
2. Find the narrowest existing test that describes it.
3. Add a failing test before changing production code.
4. Implement one vertical slice.
5. Run the focused test.
6. Run all Debug tests.
7. Run ASan/UBSan for ownership, parsing, persistence, or networking changes.
8. Update protocol/architecture/persistence documentation when behavior changes.

Example:

```bash
cmake --build --preset debug -j
./build/debug/keyforge-tests "your focused test name"
ctest --preset debug --output-on-failure
```

When adding a command, update all relevant layers:

1. Add a `CommandId`.
2. Add registry metadata and arity.
3. Add queue-time syntax validation if malformed arguments can be detected without
   database state.
4. Implement runtime semantics and WRONGTYPE behavior.
5. Decide missing-key and empty-container behavior.
6. Call `touch`, `replace`, or `erase` exactly when state changes.
7. Mark mutation metadata so canonical AOF logging occurs.
8. Decide whether the command is pub/sub-safe.
9. Add unit, transaction, persistence, and integration coverage as applicable.
10. Update README and protocol documentation.

## Improvement backlog

The following ideas are grounded in the current implementation. They are ordered by the
value they would add, not by how impressive they sound.

### Priority 0: memory and failure boundaries

#### Bound transaction queue memory

The parser input buffer is bounded, but MULTI can accumulate validated argv values across
many commands after their wire bytes have been consumed. Add a per-session queued-byte
counter and configurable transaction limit. Test that exceeding it marks the transaction
invalid without affecting other clients.

#### Bound graph edges and subscriptions

Client buffer limits do not cap graph edges or long-lived channel memberships accumulated
across commands. Add server-wide graph-edge limits plus per-connection subscription
limits, expose them in configuration, and test failure without partial mutation.

#### Inject AOF I/O failures

`Sink` makes engine persistence testable, but `Aof` directly calls `write`, `fdatasync`,
and `ftruncate`. Introduce a narrow file-operations interface or fault-injection hooks.
Test short writes, EINTR, ENOSPC, fsync failure, and truncation failure deterministically.
Then define whether a persistence failure should place the server in a fail-stop write
state rather than allowing later mutations.

#### Restore signal masks for embedded use

The process server intentionally blocks SIGINT/SIGTERM for `signalfd`, but the previous
mask is not restored because the executable owns process lifetime. If `Server` is to be
embedded in tests or another program, store and restore the old mask through RAII.

### Priority 1: split the command engine

`src/commands/engine.cpp` is approximately 1,250 lines and is the clearest maintainability
pressure point. Split handlers by domain:

```text
src/commands/string_commands.cpp
src/commands/list_commands.cpp
src/commands/hash_commands.cpp
src/commands/set_commands.cpp
src/commands/zset_commands.cpp
src/commands/transaction_commands.cpp
src/commands/pubsub_commands.cpp
src/commands/graph_commands.cpp
```

Keep lookup, queueing, persistence orchestration, and session policy in `Engine`. Consider
storing a handler function in `CommandSpec` so the registry is the single dispatch source.
The current `transaction_control` metadata is descriptive while several controls are
still dispatched explicitly; refactoring can make that metadata enforce behavior.

Do this without changing command replies. A successful refactor should pass the existing
suite unchanged before adding features.

### Priority 1: avoid repeated buffer compaction

`Parser::next` erases consumed bytes from the front of `std::string`, which moves retained
pipelined data. The server output buffer also occasionally erases a sent prefix before
appending more output. Replace these with a read cursor and compact only after a threshold,
or use segmented output chunks. Benchmark before and after with deep pipelines; do not
claim improvement from code shape alone.

### Priority 1: make active expiry timer-driven

The event loop wakes at most every 100 ms and processes 64 expiry records. Lazy expiry is
exact, but untouched keys may remain physically present until an active turn. Expose the
next valid expiry deadline and integrate Linux `timerfd`, while retaining a work budget.
Account for stale top records before arming the timer.

### Priority 1: control stale expiry-heap growth

Repeated PEXPIRE/PERSIST updates intentionally leave stale records. Correctness is sound,
but heap memory can grow far beyond the number of live TTL keys. Track live TTL count and
rebuild the heap when stale-to-live ratio crosses a measured threshold. Add a test that
performs many TTL replacements and verifies both semantics and bounded heap size through
an introspection hook.

### Priority 2: AOF rewrite

Implement background or fork-assisted logical rewrite into a temporary file, followed by
flush, atomic rename, and directory sync. The hard part is merging mutations that occur
during rewrite without blocking the event loop for the whole operation. Specify crash
points first and add recovery tests around every rename/sync boundary.

### Priority 2: ranked sorted-set access

`SortedSet::range` advances a tree iterator from the beginning, so a high start offset is
O(start). An indexed skip list or order-statistics tree would improve rank access but adds
substantial invariant complexity. Add representative benchmarks before choosing a new
structure, and preserve score/member tie ordering in property tests.

### Priority 2: stronger graph scaling

Cycle checks run reachability per proposed update, which is appropriate for the current
educational scale. For very large or mutation-heavy DAGs, investigate maintained
topological order, batched dependency registration, and memory-efficient adjacency
storage. Measure real graphs; a dynamic topological algorithm is not automatically a win.

### Priority 2: richer protocol compatibility

Good incremental extensions include:

- glob character classes and escaping for KEYS;
- ZRANGE score modes and more ZADD options;
- pattern pub/sub;
- INFO sections and more operational counters;
- RESP3 as a separate negotiated codec layer.

For each extension, add compatibility tests against a real Redis version and clearly
record intentional differences.

### Priority 2: observability

INFO currently exposes a small honest metric set. Add counters for accepted/current/
rejected connections, expired keys, cascade sizes, pub/sub deliveries and dropped slow
clients, AOF bytes/sync failures, parser errors, and command latency buckets. Keep metric
updates on the owning thread or use carefully chosen atomics for the fsync worker.

### Priority 2: deeper testing

- Add property-based state-machine tests for each data type.
- Run a differential command subset against Redis and compare normalized replies.
- Add an engine fuzz target that turns arbitrary RESP arrays into commands.
- Add fault-injection recovery tests for every AOF syscall failure.
- Add a TSan CI job focused on everysec append/flush/shutdown races.
- Add a slow-subscriber test that fills the real kernel send buffer, not only the
  configured application output cap.
- Add long randomized transaction/WATCH histories across multiple client sessions.

## Suggested improvement projects by difficulty

### Beginner: add a read-only command

Implement `GETRANGE` or a similarly bounded string command. Focus on registry metadata,
integer parsing, negative indexes, binary output, WRONGTYPE, tests, and docs.

Definition of done: unit and real-socket tests pass; no persistence record is emitted.

### Intermediate: transaction memory limit

Add configuration, session accounting, queue rejection, INFO metrics, integration tests,
and documentation. This crosses config, engine, session lifetime, and defensive design
without changing storage structures.

Definition of done: a malicious transaction cannot grow unbounded, DISCARD/EXEC/
disconnect release accounting, and normal pipelines remain unaffected.

### Intermediate: parser cursor compaction

Replace front erasure with a consumed cursor. Preserve `consumed_bytes`, pipelining, buffer
limits, reset behavior, and every-byte fragmentation tests. Add a benchmark with a large
pipeline to establish whether copies decreased.

Definition of done: all parser/recovery tests pass under ASan/UBSan and measurements are
recorded reproducibly.

### Advanced: AOF rewrite

Design a crash-safe state machine before coding. Account for expiry deadlines, every data
type, graph edges, concurrent event-loop mutations, temporary-file cleanup, atomic rename,
and directory durability.

Definition of done: fault-injection tests prove that every interrupted phase starts from
either the old valid AOF or the new valid AOF, never a mixture.

### Advanced: timerfd expiry scheduler

Integrate the next monotonic heap deadline into epoll. Handle stale heap records, earlier
TTL replacement, empty heap, bounded work, shutdown, and fake-time unit tests separately
from real timer integration tests.

Definition of done: untouched due keys are actively removed without a fixed 100 ms poll,
and expiration waves still cannot starve client I/O.

## What not to “improve” without evidence

- Do not add a command thread pool. It would invalidate the simple atomic ownership model
  and require synchronization across storage, WATCH, graph, pub/sub, and AOF ordering.
- Do not switch to edge-triggered epoll merely for sophistication. Level-triggering is
  correct and easier to audit; measure a real bottleneck first.
- Do not replace standard containers blindly. Their behavior is clear and the current
  project has no benchmark evidence showing allocator or hashing dominance.
- Do not swallow AOF corruption to improve availability. Silent partial recovery is a
  data-integrity regression.
- Do not claim Redis compatibility for a new command until reply shapes, missing keys,
  errors, and transaction/persistence behavior have tests.

## Seven-session study plan

### Session 1 — build, config, RAII

Read CMake, presets, `main`, config, `Fd`, and `Clock`. Run help/version/config failures.

### Session 2 — RESP

Read the protocol header and implementation. Run fragmentation and malformed-input tests.
Hand-trace a nested array split at three different byte positions.

### Session 3 — epoll

Read server initialization and connection methods. Use strace for PING, pipeline, QUIT,
and Ctrl+C. Draw connection state before/after a partial write.

### Session 4 — storage and expiry

Read Database and SortedSet. Run data-type, range, stale-expiry, and wall-jump tests. Write
down every operation that must bump a version.

### Session 5 — engine and transactions

Read registry/helpers, then one command family at a time. Trace queue-time versus runtime
errors and reproduce a two-client WATCH conflict.

### Session 6 — graph, pub/sub, AOF

Prove graph direction/cycle detection, follow a publication into another connection's
output queue, and hand-decode an AOF batch.

### Session 7 — tests and one change

Read integration/recovery support, run ASan/UBSan, choose one beginner/intermediate
improvement, add a failing test, implement it, and update the docs.

## Final comprehension checklist

You understand KeyForge deeply when you can answer all of these without opening the code:

- Why can one TCP read contain zero, one, or many complete commands?
- Why does parser completion return an offset instead of erasing during recursive parse?
- Why do epoll events store connection tokens rather than fds?
- Which object owns every mutable subsystem, and which thread may access it?
- When is EPOLLOUT enabled and disabled?
- How does output backpressure affect a publisher versus a slow subscriber?
- Why does `SET` clear an old TTL while INCR preserves it?
- Why are versions stored separately from entries?
- What exact event makes an absent watched key conflict?
- Why do expiry records need generation tokens?
- Why are both Unix and monotonic deadlines stored?
- Why does parent expiry call graph invalidation rather than ordinary erase?
- How does cycle detection prove an added edge is safe?
- Why is a diamond dependent deleted once?
- Why does AOF canonicalize relative TTL after executing SET?
- What makes a truncated compound AOF mutation non-applicable?
- Why is expiry paused during replay?
- Which fsync mode can lose what data?
- Why is PUBLISH excluded from persistence?
- What must be updated to add a new mutating command correctly?

If an answer is unclear, return to the corresponding pass and write a focused test. In a
systems project, the fastest route from “I think I understand” to real understanding is
usually an executable invariant.
