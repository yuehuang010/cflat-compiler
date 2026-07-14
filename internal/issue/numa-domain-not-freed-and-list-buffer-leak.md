# NumaDomain (and its worker-list buffer) is never freed - missing `move` on acquire()

Created: 2026-07-13 (found while triaging the `--heap-audit` macOS sweep of `Test/*.cb`)

## Summary

`NumaDomain.acquire()` (`cflat/core/numa.cb:175`) returns a plain `NumaDomain*`
(no `move`), so the compiler never marks the caller's local as an owning
pointer and never auto-frees it at scope exit. `NumaDomain.release()` (the
documented teardown method, `numa.cb:216`) drains and frees the hand-out
`_threads` list's ELEMENTS but does not free the `NumaDomain` struct itself,
nor does it free `_threads`'s own backing buffer (that only happens inside
`~NumaDomain` -> the struct's synthesized full destructor, which chains into
`~list<NumaThread*>`). Since nothing ever calls `delete` on the domain
pointer, `~NumaDomain` never runs, so both the 56-byte `NumaDomain` struct
and (when the domain handed out at least one thread) the `list<NumaThread*>`
backing buffer leak for the lifetime of the process.

Observed in `Test/test_threadpool.cb`'s `testNumaDomain()`, which acquires 4
domains (`dn`, `d`, `d2`, `d3`), calling only `->release(false)` /
`->release(true)` on each - never `delete`:

- 4 x 56 bytes - the `NumaDomain` struct itself (`dn`, `d`, `d2`, `d3`)
- 256 bytes - `d._threads`' backing buffer (grown to hand out `cores` threads)
- 32 bytes - `d3._threads`' backing buffer (grown for its one sleeper thread)

(`dn` and `d2` never call `acquireThread`, so their `_threads` list never
grows past a null buffer - no separate buffer leak for those two.)

This is a DIFFERENT root cause from the already-investigated/disproven
`list-of-interface-leaks-buffer.md` (that one was a measurement artifact -
`reportLeaks()` called while a `list<IFace>` was still live in scope). Here
the list genuinely outlives the program with no owner ever running its
destructor, because the owning struct that contains it (`NumaDomain`) is
never deleted in the first place. `NumaThread*` is a pointer element type,
which is irrelevant here - the leak is the list's own `_data` array, not a
missed element free.

## Repro

Minimal (no threadpool.cb, no threads spawned):

```cflat
import "diagnostic/heap_audit.cb";
import "topology.cb";
import "numa.cb";

extern int main() {
    HeapAudit.enable();
    {
        int id0 = numa_domain_id(0);
        NumaDomain* d = NumaDomain.acquire(id0, NUMA_ISOLATE_NONE);
        if (d != nullptr) { d->release(false); }
    }
    HeapAudit.reportLeaks();
    return 0;
}
```

`cflat repro.cb -i Test/library --heap-audit -g -o repro && ./repro` reports
a 56-byte leak at `NumaDomain.acquire` (plus the unrelated, already-known
`cpu_topology()` 752-byte false positive from the `numa_domain_id` ->
`cpu_topology()` call).

Confirmed fix (verification only, not applied - do not edit core per the
review's own instructions): adding `delete d;` right after `d->release(false);`
in the repro above makes the 56-byte leak disappear. This isolates the root
cause to "nothing ever deletes the domain," not to `release()`'s internal
logic being wrong.

## Root cause

`static NumaDomain* acquire(int id, int required)` should be `static move
NumaDomain* acquire(...)` to match the established idiom elsewhere in the
same codebase for owning-pointer-returning factories (e.g.
`cflat/core/threadpool.cb:442` `move TaskHandle* submit(...)`, `:531` `move
TaskHandle* then(...)`). Without `move` on the return type,
`lastCallReturnsOwned` (`LLVMBackend.h:9390`) is never set for the call
result, so the declaration `NumaDomain* d = NumaDomain.acquire(...)` never
marks `d`'s `NamedVariable.IsOwning = true`, and the scope-exit cleanup path
(`LLVMBackend.h:1718`, gated on `namedVar.TypeAndValue.Pointer &&
namedVar.IsOwning`) never runs `delete d` for it - matching the struct's own
header comment at `numa.cb:157-160` ("the returned pointer is owned by the
caller's local and auto-releases at scope exit"), which is therefore
currently false in practice.

Separately, even with `move` added, `release()` intentionally does NOT free
the struct itself (by design - `release()` is meant to be a "give back the OS
resources but keep using the handle" method, e.g. `testNumaDomain` continues
to call `d->info()`/`d->allocLocal()` etc. after some `release()` calls in
similar code elsewhere is not the case here, but `release()` is also called a
second, harmless time by `~NumaDomain` idempotently). So the actual full fix
needs BOTH: (1) `move` on `acquire()`'s return type so scope-exit auto-delete
fires, and (2) confirm every existing call site (only `Test/test_threadpool.cb`
today) is fine relying on that auto-delete rather than needing an explicit
`delete`.

## Fix direction

1. Add `move` to `NumaDomain.acquire()`'s return type in `cflat/core/numa.cb:175`.
2. Add a regression case to `Test/test_threadpool.cb`'s existing
   `testNumaDomain()` under `--heap-audit` (or a dedicated leak-audit test if
   the project already runs one) asserting zero leaks after all 4 domains
   go out of scope - once `move` is added no explicit `delete` should be
   needed.
3. Re-run the `--heap-audit` sweep on `Test/test_threadpool.cb` to confirm
   the 56 B x 4 and 256 B / 32 B leaks are gone (the unrelated 752 B
   `cpu_topology()` singleton leak is expected to remain - false positive).
