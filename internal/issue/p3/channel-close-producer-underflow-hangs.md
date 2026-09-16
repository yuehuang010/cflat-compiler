# channel<T>.close_producer() with no registered producer underflows and hangs the receiver

## Summary

`channel<T>` terminates a blocking `receive(T*)` when the producer count reaches 0. The counter
starts at 0, so a `close_producer()` that is not matched by an earlier `add_producer()`
decrements 0 -> -1, never hits the `== 0` test, and the channel is never marked closed. Every
`receive(T*)` on it then spins forever at 100% CPU with no diagnostic.

This is easy to hit: a results/completion channel is naturally only ever *received* from by the
owner, so a user who calls `close_producer()` to say "no more results" without having called
`add_producer()` gets a silent livelock. Found 2026-09-16 while writing a worker pool from the
docs (`scratch/dogfood/sys/pool.cb`).

## Repro

`scratch/dogfood/sys/repro_1.cb`:

```c
import "thread.cb";
import "channel.cb";

extern int main()
{
    channel<int> ch;
    ch.init(8);
    ch.close_producer();                                // unmatched: _producers 0 -> -1
    printf("is_closed=%d\n", ch.is_closed() ? 1 : 0);   // prints 0
    int v = 0;
    ch.receive(&v);                                     // spins forever
    return 0;
}
```

```
x64/Release/cflat scratch/dogfood/sys/repro_1.cb -o scratch/dogfood/sys/repro_1 -i Test/library
timeout 6 ./scratch/dogfood/sys/repro_1
is_closed=0      exit 124 (timeout)
```

## Observed

`is_closed()` stays false; `receive(T*)` never returns.

## Expected

An unmatched `close_producer()` should close the channel (clamp at 0), or abort with a
`LogError`-style runtime message naming the imbalance. Spinning forever is the worst outcome.

## Root cause

`cflat/core/channel.cb`, `close_producer()`:

```c
i64 remaining = __atomic_counter_decrement(&_producers);
if (remaining == (i64)0) { __atomic_release_store_i64(&_closed, (i64)1); }
```

`remaining` is `-1`, so the close store never runs. `is_closed()` reads `_closed`, still 0.

## Fix direction

Change the test to `remaining <= (i64)0` in `close_producer()` so an unmatched close still
closes. Optionally assert/abort on a negative count in a debug path. Consider the same audit for
`arena_channel.cb`, which mirrors the producer-lifetime scheme.

Secondary, worth a separate look: `receive(T*)` is a pure spin loop (`pause()` + yield every
1024 iterations), so a stalled channel pegs a core. `doc/THREADING.md` calls `channel<T>` a
"blocking MPMC queue", which reads as "sleeps on a condvar".
