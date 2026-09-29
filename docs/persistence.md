# Append-only persistence

## Record format

`data-dir/appendonly.aof` is a concatenation of RESP2 command arrays. Every durable unit
has this form:

```text
MULTI
<one or more canonical mutating commands>
EXEC
```

The file is intentionally inspectable with RESP tooling. One client command produces one
batch; one user EXEC produces a batch containing all successful mutating effects. Reads,
failed mutations, and pub/sub messages are omitted.

SET NX/XX conditions are resolved before logging. Successful SET logs the final SET, and
EX/PX logs a second absolute `PEXPIREAT`. EXPIRE/PEXPIRE also become PEXPIREAT. Therefore
restart never resets a relative countdown. Graph registration and invalidation commands
use the same batching path.

## Fsync modes

| Mode | Acknowledgement and crash window |
|---|---|
| `always` | The entire batch is written and `fdatasync` succeeds before the client receives success. |
| `everysec` | The batch reaches the kernel before success; a worker syncs approximately once per second, so a crash may lose the latest interval. |
| `no` | The batch reaches the kernel; no explicit sync is requested and OS writeback determines durability. |

The every-second worker and event loop serialize descriptor operations with one mutex.
It avoids an fsync for every command but may briefly delay an append while the periodic
sync holds that mutex. Graceful shutdown joins the worker and syncs dirty data for
`always`/`everysec`. Disk-full or sync failure returns `MISCONF`; because execution
precedes canonicalization, the memory change can already exist.

## Replay and corruption policy

Startup opens the file before binding TCP, streams it through the normal RESP parser, and
executes commands through the normal engine with persistence and live publication
disabled. A replay command error is fatal. Expiry checks are deferred during replay;
after all graph edges exist, due deadlines are processed and cascade normally.

The reader tracks byte offsets at completed outer EXEC boundaries. If EOF interrupts the
final RESP frame or leaves a MULTI without EXEC, the tail is assumed to be the final
interrupted write, discarded in memory, and trimmed from disk. An unknown type byte,
invalid CRLF/length, unmatched EXEC, semantic replay error, or corruption encountered
before EOF stops startup with an actionable byte-offset message. KeyForge never silently
skips a corrupt middle record.

## Limitations

There is no AOF rewrite/compaction, checksum, encryption, cross-machine replication, or
RDB snapshot. Atomic batches prevent partial command application during replay, but the
filesystem and storage hardware still define the ultimate guarantees of `fdatasync`.
