# doc/THREADING.md lists channel<T> methods that do not exist (`recv`, `tryRecv`)

## Summary

`doc/THREADING.md` line 301, in the "Other Synchronization Primitives" list:

> - `core/channel.cb` - `channel<T>` blocking MPMC queue; `send`, `recv`, `tryRecv`

Neither `recv` nor `tryRecv` exists. `cflat/core/channel.cb` declares `send`, `receive()`,
`receive(T*)`, `try_receive(T*)`, `push`, `pop`, `try_push`, `try_pop`, plus the producer
lifetime pair `add_producer` / `close_producer`. The code example six lines below the bullet
correctly uses `receive()`, so the summary line is the only wrong text - but it is the line a
reader scans first, and `ch.recv(...)` fails with "Unknown identifier".

Found 2026-09-16 while writing a worker pool from the docs.

## Observed

`ch.recv(&v)` / `ch.tryRecv(&v)` do not compile.

## Expected

The bullet should read `send`, `receive`, `try_receive` - and ideally mention
`add_producer`/`close_producer`, which are mandatory for the blocking `receive(T*)` to ever
terminate but appear nowhere in the bullet or the short example. A user who writes the
three-line example and then adds a consumer thread has no way to learn from that section that
the channel must be opened and closed.

## Fix direction

Doc-only. Fix `doc/THREADING.md` line 301 and add the producer-lifetime pair to the
`channel<T>` example; `internal/stdlib-reference.md` line 72 is correct as a one-liner but could
carry the same three method names.
