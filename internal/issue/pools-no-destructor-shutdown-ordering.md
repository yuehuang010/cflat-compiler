# The pools (`block_pool` / `arena_channel` / `page_pool`) stay manual - deliberately

Filed 2026-07-20 while executing the RAII ruling (`internal/plan/unique-ownership.md` NEXT
item 4). These three were explicitly scoped as "treat last and be conservative; if adding a
destructor is not clearly safe, report rather than force it." This is that report.

## What they gained anyway

`block_pool<T>` holds a `BucketAllocator` (`arena_channel.cb:52`), which holds a `mutex`
(`bucket_allocator.cb:221`); `arena_channel<T>` holds a `block_pool<T>` (`:179`). Since
`mutex` now has a destructor, all three ALREADY release their lock state automatically at
scope exit through synthesized memberwise destruction. Only the PAGE/ARENA teardown is still
manual.

## Why no `~arena_channel()` / `~block_pool()`

`arena_channel.destroy()` (`arena_channel.cb:246-257`) carries a precondition a destructor
cannot check:

> `// Call only after all producer threads have finished and the ring is drained.`

`destroy_arenas()` says the same. A destructor that fires at scope exit while a producer
thread is still holding a `page_arena<T>*` is a use-after-free, not a leak. This is the same
hazard shape as `~Thread()` (see `thread-cannot-go-raii.md`): the type's teardown is
correct only at a moment the owner knows and the compiler does not.

## Why no `~page_pool()`

`g_page_pool` (`page_pool.cb:165`) is a PROCESS-GLOBAL cache of 64 KB pages. Two reasons to
leave it:

1. **It is not a leak in the meaningful sense.** `cleanup()` `vmem_free`s cached pages; at
   process exit the OS reclaims them regardless. The destructor would buy tooling silence,
   not correctness.
2. **It adds shutdown-ordering coupling for no gain.** Measured 2026-07-20: cflat runs
   global destructors after `main`, in REVERSE declaration order, and imported-library
   globals are declared first and so destroyed LAST. That ordering happens to be favourable
   today (a user-file `arena_channel` global tears down before `g_page_pool`), but it is an
   emergent property of import order, not a guarantee anyone stated. Making correctness
   depend on it is a trap for whoever next reorders an import.

## If this is revisited

The precondition is the whole problem. A defensible RAII story needs the pool to KNOW it is
quiescent - e.g. an outstanding-arena counter that the destructor asserts is zero, turning a
silent use-after-free into a diagnostic. That is a design change, not a destructor.

## Related

- `internal/plan/unique-ownership.md` NEXT item 4.
- `thread-cannot-go-raii.md` - same "teardown is only correct at a moment the compiler
  cannot see" shape.
