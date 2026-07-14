# Lock guards: core is converted; raw acquire/release is NOT blocked

**Status: core converted, enforcement deliberately NOT enabled.**

Every lock in `core/` is now taken through the scoped `lock(...)` statement, but
calling `mutex.acquire()` / `release()` directly remains legal. A compiler check that
rejected raw calls was built and then removed on purpose - see "Why not blocked" below.

## What was done

**All 80 raw call sites in `core/` were converted to `lock()` blocks**: arena_allocator
10, bucket_allocator 10, barrier 5, numa 17, stream 20, threadpool 8. Every site was
lexically balanced inside a single function - there was no non-lexical lock or hand-off
anywhere in the tree - so the conversion was mechanical and behavior-preserving.

Two `lock()` capabilities made it possible, both verified first:
- An early `return` from inside a `lock()` block DOES release the mutex. Needed by
  `barrier.arrive_and_wait`, `numa.release`, and stream's `release(); return nullptr;` sites.
- `mutex.sleep_cv()` is callable inside a `lock()` block and the lock is still held
  afterwards. Needed by stream's 10 condvar loops, plus barrier and threadpool.

**`lock(rw.read)` / `lock(rw.write)` was fixed** (kept - this was a real bug). The mode
branch of the lock statement was an MVP stub that evaluated the full `rw.read` expression,
hit the nonexistent `.read` field, and failed with "must be a mutex variable". Shared read
locks had never worked, despite being documented. `ParsePostfixExpression` gained a
`dropTrailingChildren` parameter, and the mode branch now evaluates the postfix chain minus
the trailing `.read`/`.write` member access (`rw.read` -> `rw`, `a.rw.read` -> `a.rw`,
`p->rw.read` -> `p->rw`).

**File-scope guard groups are now supported**: `lock(g_mtx) { <globals> <functions> }` at
file scope guards globals with a global mutex, with the same semantics as the struct form
(guarded globals report "Global 'x' is guarded by 'g_mtx'"; group functions gain
`RequiredLocks = {g_mtx}`). Both bare and namespace-qualified access (`N.g_count`) are
checked.

**Adopted in `core/numa.cb`**: the four process-wide registry globals
(`_g_numaAcquiredIdx`, `_g_numaConfineIdx`, `_g_numaSavedCpus`, `_g_numaHaveSaved`) are in a
`lock(_g_numaRegLock)` group, and `_numa_recompute_confinement` carries a
`lock(_g_numaRegLock)` clause - its "Caller holds _g_numaRegLock" prose comment is now a
checked contract.

**Deliberately NOT adopted in `arena_allocator.cb` / `bucket_allocator.cb`.** Their radix
registries are lock-free on read by design: `_ar_reg_lookup` / `_ba_reg_lookup` sit on the
`free()` hot path and take no lock, and the lock guards only *publication* of a second-level
table (double-checked locking; x64 TSO means a thread observing a published `_table` also
observes its zero-fill). A guard group means "hold the lock to TOUCH this", so it cannot
express that read/write asymmetry - it would reject all ~10 fast-path reads, and satisfying
it would serialize every `free()` through a global mutex. A guard group needs a
read-lock-free / write-locked variant before these could adopt it.

Known soundness limit: the lock-set is a set of *name strings*, so a held lock satisfies
any guard spelled the same way. `lock(N.g_mtx)` also contributes the bare `g_mtx` (a group
inside `namespace N` names its guardian as written there), so an unrelated guard named
`g_mtx` in another scope would be satisfied by it. This is the same approximation the
existing `this.mtx` -> `mtx` aliasing already makes; it yields false negatives (a missed
error), never a false positive.

## Why not blocked

**A concurrent B-tree for HPC is a planned direction, and it needs hand-over-hand lock
coupling** (hold `cur`, acquire `next`, release `cur`, advance). That requires FIFO release
order. Scoped `lock()` blocks release in LIFO order only, so hand-over-hand is
*inexpressible* with nested lock blocks - nesting holds every lock down the chain until
unwind, which is a different and much worse algorithm. Blocking raw acquire/release would
have walled off a B-link tree implementation.

Two other patterns the block would have foreclosed:
- **Returning a held lock** (guard-as-value / "locked accessor" API). The lock's lifetime
  becomes exactly one lexical block.
- **Try-lock / timed lock.** Neither exists today, but `if (m.try_acquire()) { ...; m.release(); }`
  is exactly the shape a block would reject. Adding one would require a `lock (m) else { }` form.

## If enforcement is ever revisited

The check was ~25 lines (`CheckRawLockCall`, next to `CheckCallSiteLocks` in
`MainListener.h`), called from the two method-call sites that already call
`CheckCallSiteLocks`. Notes for a future attempt:

- It must be **type-directed, not name-directed**. `acquire`/`release` are also method
  names on `semaphore` (25 sites), the page pool (16 sites), and `NumaDomain`'s static
  factory (14 sites). A name-based check produces 55 false positives.
- **Type classification now exists**: `[Capability(ILockable)]` marks a lock type in source
  (`internal/plan/lock-capability-interface.md`, landed). The page pool and `NumaDomain` carry
  no capability, so they are excluded for free. **But `semaphore` does carry one** - `lock (s) { }`
  is existing tested behavior (`Test/test_sync.cb:599`) - while a semaphore is *also* a counting
  handoff, acquired on one thread and released on another, which is non-lexical by nature.
  So "is a lock" and "must be taken via `lock()`" are **orthogonal**, and a capability marker
  alone is not enough to gate the check. A second marker (e.g. `[ScopedLock]` on `mutex`/`rwlock`
  but not `semaphore`) would be required.
- No exemptions are needed: the `lock()` statement lowers via
  `CreateOverloadedFunctionCall` and does not go through the method-call expression path;
  `core/mutex.cb` and `core/rwlock.cb` bodies call `os.*` directly and never self-call;
  `condvar.wait(mutex*)` reaches into `&m->_srw` rather than calling `m->acquire()`.
- **Do not hardcode the type names in C++.** A check keyed on `"mutex"`/`"rwlock"` means
  every future lock type (recursive_mutex, spinlock, seqlock) needs a compiler edit, and
  a user-defined lock type silently escapes it (verified: a `struct MyLock` wrapping
  `os.mutex_lock` compiles unchecked). Make it a source-level property of the struct.
  **DONE** - `[Capability(...)]` is that property, and the `lock()` statement no longer
  contains a lock type name or method name. See `internal/plan/lock-capability-interface.md`.
- Any such rule needs an escape hatch for hand-over-hand locking, or the concurrent
  B-tree cannot be written.
