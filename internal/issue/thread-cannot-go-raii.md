# `Thread` cannot take a destructor - two independent blockers

Filed 2026-07-20 while executing the "core manual-lifecycle types go RAII" ruling
(`internal/plan/unique-ownership.md` NEXT item 4). Every other type named by that ruling
migrated; `Thread` is the one that did not, and the reasons are structural rather than
incidental.

The ruling's own escape clause applies: *"If a copy path DOES exist for a type, do not add
its destructor - report it instead."* This is that report.

## Blocker 1 - `Thread` is COPIED by value, by design, in `ThreadPool.resize()`

`cflat/core/threadpool.cb:659` (grow path):

```cflat
Thread* nw = new Thread[newCount];
...
while (i < oldCount) { nw[i] = _workers[i]; ni[i] = _workerIds[i]; ns[i] = _workerStops[i]; i++; }
delete[_] _workers;
```

`nw[i] = _workers[i]` is a value assignment of a `Thread` holding a LIVE `_handle` and
`_args`, immediately followed by `delete[_] _workers` on the source array. The code says so
itself at `:649-650`:

> `// Thread and stop_source have no destructors, so bitwise copy`
> `// is safe: Win32 handles and heap _state pointers remain valid.`

With a destructor this becomes a double release of one OS thread handle per live worker on
every grow that exceeds capacity.

## Blocker 2 - the ruling's semantics conflict with a shipping DETACH pattern

The ruling is that `~Thread()` on a still-running thread is an ERROR. But core deliberately
detaches a running thread from a scope-local `Thread`:

`cflat/core/channel.cb:344-351`, `channel<T>.operator>>`:

```cflat
Thread t = default;
t.start(entry, (void*)ctx);              // detached fire-and-forget forwarder
}                                        // t scope-exits while the thread runs
```

Every `a >> b` channel pipe does this. The forwarder must outlive the local by design - it
drains the source until the last producer closes.

This is not a copy path, and it is worth separating from Blocker 1: even if `Thread` were
made non-copyable tomorrow, the error-on-still-running destructor would still be wrong for
this pattern.

## Verified empirically, not inferred

A `~Thread()` matching the ruling (error + `abort()` when `_handle != nullptr`) was added
experimentally and the suite run. `test_sync` aborted with `~Thread: destroyed while still
running.`, isolated to `testChannelPipeSingle` - i.e. Blocker 2 fires first and on a common
path. Suite went 448/0/8 -> 447/1/8. The experiment was then reverted.

## Fix direction

`Thread` needs an explicit lifecycle DISPOSITION before it can be RAII, not just copy
suppression. Roughly the C++ shape:

1. Make `Thread` non-copyable (needs the `unique`-for-non-`delete`-releases extension the
   maintainer named as the long-term answer, or an equivalent). This alone unblocks
   Blocker 1 - `resize()` would move rather than copy.
2. Give the detach case a NAME. `channel.operator>>` should call an explicit
   `t.detach()` that nulls `_handle` and transfers responsibility, so scope exit is
   legitimately a no-op and the error-on-still-running destructor stays honest. Today
   "detached" is spelled "just let the local die", which a destructor cannot distinguish
   from the mistake it is meant to catch.

Until both exist, `Thread` teardown stays manual (`join()` / `try_join()` / `terminate()`).

## What WAS fixed

The double-`start()` half of the original report landed:
`Thread.start()` (both overloads) now rejects a second call on a started thread with a
diagnostic and `abort()`, instead of leaking the prior handle and `_args` and abandoning a
running thread that could never be joined. See `cflat/core/thread.cb`.

## Related

- `internal/plan/unique-ownership.md` NEXT item 4 - the ruling and the copy-suppression
  problem it is working around.
