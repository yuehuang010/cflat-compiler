# `channel<T>` / `spsc_queue<T>` strand UNDELIVERED elements on destroy

Filed 2026-08-22, found while reviewing the container `_drainFresh` fix (104d62a). That fix
covers the *construction* half of the ring-buffer ownership contract; this is the *teardown*
half and is a separate defect.

## Repro

```cflat
import "spsc_queue.cb";          // identical with channel.cb / channel<Cell>
int live = 0;
struct Res  { int v = 0; Res() { live++; } ~Res() { live--; } };
struct Cell { unique Res* p = new Res(); };
extern int main()
{
    live = 0;
    {
        spsc_queue<Cell> q = default;
        q.init(4);
        Cell a = default; q.push(move a);
        Cell b = default; q.push(move b);
        printf("in-scope live=%d\n", live);
    }
    printf("after destroy live=%d\n", live);
    return 0;
}
```

Output on both binaries: `in-scope live=2`, `after destroy live=2`.
`leaks --atExit`: `2 leaks for 32 total leaked bytes`. Same numbers for `channel<Cell>`.

## Root cause

`spsc_queue.destroy()` (`cflat/core/spsc_queue.cb:140`) and `channel.destroy()`
(`cflat/core/channel.cb:381`) free the ring with `delete[_] _buf`, which releases the raw
storage WITHOUT running any element destructor. Neither walks the live range
(`[_tail, _head)` for spsc_queue, `[tail, head)` derived from `_seq` for channel), so any
element pushed but never popped is stranded. `~spsc_queue` / `~channel` just call `destroy()`,
so scope exit strands them too.

This is the mirror image of the `_drainFresh` bug: that one leaked because fresh slots were
LIVE when the container assumed they were empty; this one leaks because live slots are
discarded when the container assumes they were already handed off.

## Fix direction

Before the `delete[_] _buf` in `destroy()`, release every undelivered slot the same way the
other containers do - `_ = move _buf[slot]` over the live range - guarded by the same
`if const (!is_primitive(T) && !is_pointer(T))` gate `_drainFresh` uses, then reset
head/tail so a second `destroy()` is still a no-op (both are documented as idempotent).

Concurrency caveat: `destroy()` has no synchronisation, so the drain is only sound once all
producers/consumers have quiesced - which is already the documented precondition for calling
it. A `channel` closed and drained by its forwarder thread has an empty live range and pays
nothing.

Regression leg: the repro above, added to the Bug 11 block in
`Test/test_collection_leaks.cb`, asserting `live == 0` after teardown plus the HeapAudit
oracle.
