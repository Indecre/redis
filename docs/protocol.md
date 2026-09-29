# Protocol and command contract

## RESP2 subset

The wire codec supports RESP2 simple strings (`+`), errors (`-`), signed 64-bit integers
(`:`), bulk strings (`$`, including `$-1`), and arrays (`*`, including `*-1`). CRLF is
mandatory. Payloads and keys are binary-safe; command names are compared as ASCII without
case sensitivity.

Clients must send a non-empty array whose elements are bulk strings. Inline syntax,
RESP3 attributes/maps/push types, and mixed-type command arrays are not accepted. The
parser applies configured caps to bulk length, array/argument count, nesting, header
lines, and total buffered input. One TCP read may contain a fragment, one frame, or many
pipelined frames.

## Reply conventions

- Successful setters/control commands generally return `+OK`; transaction-queued
  commands return `+QUEUED`.
- Missing scalar values return a null bulk string. EXEC conflicts return a null array.
- Counts and booleans are RESP integers.
- Collections and EXEC replies are arrays. Graph inspection is lexicographically sorted;
  HGETALL and SMEMBERS are also stable in this implementation.
- Wrong-type access is `-WRONGTYPE ...`. Syntax, arity, numeric, and protocol failures are
  RESP errors.
- SUBSCRIBE/UNSUBSCRIBE acknowledgements and published messages use Redis-style arrays:
  `['subscribe', channel, count]`, `['unsubscribe', channel-or-null, count]`, and
  `['message', channel, payload]`.

## Commands

The complete implemented set is listed in the README. Notable shapes are:

```text
SET key value [NX|XX] [EX seconds|PX milliseconds]
LRANGE key start stop
ZADD key score member [score member ...]
ZRANGE key start stop [WITHSCORES]
WATCH key [key ...]
KFG.DEPEND child parent [parent ...]
```

List/zset stop indexes are inclusive; negative indexes count from the end. Zset order is
score then lexicographic member and NaN scores are rejected. `KEYS` implements `*` and
`?` only. The internal `PEXPIREAT key unix-milliseconds` form exists for AOF replay.

## Intentional incompatibilities

KeyForge is a Redis-compatible subset, not a compatibility clone. It has one database,
no inline protocol, no RESP3, no authentication/ACL/TLS, no scripting, no cluster or
replication, no pattern pub/sub, and a deliberately small command option surface. INFO
contains KeyForge-specific honest metrics rather than Redis's full schema. Container
iteration is deterministic in several places where Redis does not promise ordering.
