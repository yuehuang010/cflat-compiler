# `WaitForExit(stop_token)` hangs forever on a program whose thread was never started

Filed 2026-08-09 while fixing `program-run-without-move-list-arg-miscompiles` (the
`Thread.join()` null-packet crash). Found by enumerating the other "operate on a
never-started program thread" cells; this one is a hang, not a crash, and is a
different mechanism, so it was left out of that fix.

**Severity: hang, zero output, no diagnostic.**

## Repro

```cflat
import "list.cb";
program P { int main(move list<string> args) { return 3; } };
extern int main() {
    P p; stop_source ss;
    bool w = p.WaitForExit(ss.get_token());
    printf("w=%d\n", w);
    return 0;
}
```

Measured on macOS arm64 Release, before and after the `join()` fix alike: no output,
killed by a 15s timeout (rc 124).

## Root cause

`__wait_thread_or_stop` (`cflat/core/stop_token.cb:55`) polls `t->try_join(50)` until the
token is cancelled. `Thread.try_join` (`cflat/core/thread.cb`) calls
`os.thread_timed_join(_handle, ...)` with a null `_handle`, which sets `fin = false`, so
the loop never exits and the token is never cancelled.

The sibling spellings on the same program state all return promptly: `WaitForExit()`
returns immediately (after the `join()` packet guard), `WaitForExit(int)` returns false,
`Kill()` and `RequestStop()` are no-ops.

## Fix direction

Give `try_join` the same "no packet means nothing to wait for" answer `join()` now has -
`_args == nullptr` should report the thread as finished (return true) rather than time out.
That makes `WaitForExit(stop_token)` and `WaitForExit(int)` agree with `WaitForExit()` on
an identical program state and removes the hang.

It is a behaviour change for one currently-working spelling: `P p; p.WaitForExit(50);`
returns `false` today and would return `true`. That is why it was not folded into the
`join()` fix, which changes no program that runs correctly today. Freeze that cell before
changing it.
