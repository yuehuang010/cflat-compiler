# Container buffers: construct-then-drain costs ~3.5% on list<small struct> add; calloc would be free

Filed 2026-08-22 from the review of the `_drainFresh` fix (list/queue/stack/dictionary/hashset/
channel/spsc_queue). `new T[n]` default-constructs every slot, and the drain that releases
acquiring element types lowers to a per-field zero-store loop even for a trivially-destructible
`struct { int a; int b; }`. Measured, 10M `list.add`, 20 interleaved runs: `-O2` 0.744 s ->
0.770 s (1.035x); unoptimized 1.09 s -> 1.16 s (1.064x).

## Fix direction

The seven containers never read a slot outside their live range, so their backing buffer can be
raw storage: `T* buf = (T*)calloc(n, sizeof(T))` (one memset, no ctor loop, no drain). `malloc`
provenance is already idiomatic in core (`channel.cb` `_seq`, `arena_allocator.cb`). Needs a
check that the slot-store / `move` machinery in the compiler (Part 6 of
MainListener_Expressions.cpp, the single-index GEP store rule) is happy with a malloc'd buffer,
and that teardown switches from `delete[_]` to `free` consistently. `array<T>` must KEEP
`new T[n]`: it exposes unset slots and its `delete[_len]` destructs them.

Alternative: a `has_acquiring_default(T)` compile-time intrinsic to narrow the drain gate
(`!is_copyable(T)` is NOT a valid substitute - a copyable struct whose user ctor allocates into a
raw pointer is still acquiring).

Not urgent: the cost is under the 5% review bar. Record so the number is not re-measured.
