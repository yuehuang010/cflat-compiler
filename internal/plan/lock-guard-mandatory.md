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
- No exemptions are needed: the `lock()` statement lowers via
  `CreateOverloadedFunctionCall` and does not go through the method-call expression path;
  `core/mutex.cb` and `core/rwlock.cb` bodies call `os.*` directly and never self-call;
  `condvar.wait(mutex*)` reaches into `&m->_srw` rather than calling `m->acquire()`.
- **Do not hardcode the type names in C++.** A check keyed on `"mutex"`/`"rwlock"` means
  every future lock type (recursive_mutex, spinlock, seqlock) needs a compiler edit, and
  a user-defined lock type silently escapes it (verified: a `struct MyLock` wrapping
  `os.mutex_lock` compiles unchecked). Make it a source-level property of the struct.
- Any such rule needs an escape hatch for hand-over-hand locking, or the concurrent
  B-tree cannot be written.

## Known gap: guard groups cannot be declared over globals

A file-scope guard group does not parse:

```cflat
mutex g_reg = default;
lock(g_reg) { int g_count = 0; }   // error: mismatched input 'lock' expecting end of file
```

`lock(g_reg) { ... }` as a *statement* works on a global mutex; you just cannot declare
*what it guards*. `arena_allocator`, `bucket_allocator`, and `numa` each lock a global
registry this way, so their registry state cannot be compiler-guarded. Allowing a guard
group at file scope would close this.
