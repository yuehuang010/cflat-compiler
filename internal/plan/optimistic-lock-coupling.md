# Optimistic lock coupling: `olock`, the acquire fence, and lock modes

**Status: Phases 0-5 LANDED. Phase 5 measured the OLC thesis on Apple M5 Pro and it holds
(olc/coupling ratio grows from 1.38x at 1 thread to 14.30x-119.43x at 18 threads). Phase 6
DEFERRED (reclamation). Phase 7 NOT JUSTIFIED - do not build it.**
Section numbers below match the item numbers in [Sequencing](#sequencing).

Target: a concurrent B-tree for HPC (see the memory note "Concurrent B-tree direction").
This doc works out what that needs from the *compiler*, what it needs from *core*, and what
it needs from *nobody* - and finds that the compiler's share is much smaller than expected.

Related: `internal/plan/lock-capability-interface.md` (the `[Capability(...)]` mechanism this
builds on), `internal/plan/lock-guard-mandatory.md` (why raw acquire/release stays legal).

## Why not just use `rwlock`

The textbook concurrent B-tree descends with reader-writer latches, coupling hand-over-hand.
On a many-core machine that design does not scale, and the reason is not contention - it is
cache coherence. `rwlock.acquire_read()` is a *store* to a shared word. Every lookup, on every
core, writes the root's latch line. That line ping-pongs across the interconnect and read
throughput collapses even at zero logical contention.

Optimistic lock coupling (Leis et al., "The ART of Practical Synchronization", 2016) removes
the store. A reader snapshots a version counter, reads the node, and re-reads the counter. If
it changed, the read saw a torn node and the whole operation restarts from the root. The read
path issues **zero stores**, so a hot node's cache line stays Shared in every core's L1.

This is safe only because reading garbage is *harmless*, which rests on two invariants:

1. a node is never freed while a reader might be inside it (deferred reclamation - see
   [Phase 6](#phase-6---concurrent-structural-remove-deferred-2026-07-14), which explains why
   lookup and insert need none of it), and
2. every read inside a speculative region lands within the node's fixed-size storage, so a
   nonsense index still reads in-bounds nonsense rather than faulting.

Invariant 2 is why the speculative body must have **no side effects**. That is not a style
rule; it is the thing that makes the algorithm sound, and it is the thing the compiler can
enforce and C++ cannot.

## The ordering-bracket family

CFlat does not hand out free-standing fences. Memory ordering is expressed structurally, by
which side of a lambda body the atomic sits on. `core/atomic.cb` already has two members of
this family:

```cflat
// core/atomic.cb:162 - the atomic comes AFTER the body; its ordering points back over it.
void release(i64 val, lock(this) Lambda<void()> body) {
    body();
    __atomic_release_store_i64(&_value, val);
}

// core/atomic.cb:168 - the atomic comes BEFORE the body; its ordering points forward over it.
void acquire(lock(this) Lambda<void(i64)> body) {
    i64 v = __atomic_acquire_load_i64(&_value);
    body(v);
}
```

A seqlock needs a third bracket: an atomic *after* the body whose ordering points *back over*
it - and that is the one combination the hardware does not offer. There is no "release load".
So the closing re-read has to be preceded by an explicit acquire fence. This is exactly what
the Linux kernel's `read_seqcount_retry()` does: `smp_rmb()` *before* comparing the sequence,
not an acquire load *of* it.

`lfence()` in `core/time.cb` does not fill this hole. It lowers to `llvm.x86.sse2.lfence`
(`LLVMBackend.h:3075`), is wrapped in `if const (__X86__)`, and exists to serialize `rdtscp()`
for timing. On arm64 - the primary dev target - it compiles to nothing. A seqlock built on it
would pass every test on Apple Silicon and be silently unsynchronized there.

So the single new primitive is a portable acquire fence. Everything else is `.cb`.

## Phase 0 - `btree<K,V>`, single-threaded (core)

**IN PROGRESS.** `core/hpc/btree.cb`. No locks, no atomics, no threads - not one line of the
rest of this doc. Get the split/merge/rebalance logic and the ownership contract right in a
setting where they can be reasoned about, then add concurrency to a structure already known
correct.

The tree is a **B+tree**: all key/value pairs live in the leaves, internal nodes hold only
separator keys and child pointers, and leaves are linked left-to-right. The leaf chain is not
optional even in v1 - the B-link recovery path that Phase 4 depends on (a reader that lands
left of a concurrent split walks right) is built on it, and retrofitting it would mean
rewriting the node layout.

It lives in `core/hpc/`, not `core/`, deliberately. A B+tree is the wrong default for general
use: `dictionary<K,V>` beats it on point lookups and `list<T>` beats it on iteration. It earns
its place only for ordered iteration, range scans, and many-core read scaling. Keeping it out
of `core/` makes reaching for it an explicit act rather than a reflex.

Node layout. Fixed inline arrays (precedent: `regex.cb:169`) - which is also what makes
invariant 2 above hold once Phase 4 lands, since a garbage index from a torn read still lands
in-bounds. One node type with an `isLeaf` flag, so every node carries a uniform header (Phase 4
wants the `olock` at a fixed offset); the wasted `values`/`children` overlap is a known
memory optimization, deliberately not taken.

```cflat
// core/hpc/btree.cb - v1.  Phase 4 adds `olock ver` and wraps these fields in a lock(ver) group.
int BTREE_FANOUT = 16;

struct btree_node<K, V> {
    int              count        = 0;
    bool             isLeaf       = true;
    K                keys[16]     = default;
    V                values[16]   = default;   // leaves only
    btree_node<K,V>* children[17] = default;   // internal only
    btree_node<K,V>* next         = nullptr;   // leaf chain; the B-link right pointer
};
```

Ordering comes from `operator<` on K - there is no `IComparable` in this codebase and one
should not be invented for this. Ownership mirrors `dictionary.cb` exactly (`move V` in,
`alias V` out, `if const (is_pointer(V))` value freeing, `key.copy()` for string keys); that
contract is fiddlier than the tree algorithm and is the likeliest source of bugs.

`remove()` with borrow/merge/root-collapse **is** in v1. It is only *concurrent* structural
delete that Phase 6 defers - single-threaded delete frees nodes safely because there are no
concurrent readers.

## Phase 1 - the acquire fence (compiler + core)

**LANDED 2026-07-14.** `LLVMBackend.h` `CreateFenceAcquire()`; `MainListener.h` intercept +
`kIntrinsics` named-set entry; `fence_acquire()` in `core/intrinsic.cb`; `store_release(i64)` on
`atomic__i64`. Test in `Test/test_sync.cb`. `test.sh` 167/0/15.

Verified where it counts - not "it built", but **what the machine actually executes**:

```
IR         : fence acquire
arm64 asm  : dmb ishld          <- a real load-load barrier
             stlr / ldar        <- store_release / load
```

That `dmb ishld` is the whole point. `lfence()` would have emitted *nothing* here.

**Interception path (resolved):** the name took the **named-set** path, not the `__atomic_`
prefix path, so no `extern` stub was needed. The `functionName ==` if-chain in `MainListener.h`
is evaluated on the textual name *before* any function-table lookup, and short-circuits; the
`__atomic_` prefix intercept (`LLVMBackend.h:13454`) only fires *after* a candidate is resolved
via `GetFunction`, which requires a declaration. The named set wins by construction. The plan
flagged a possible collision here; there was none.

The original design notes follow.

```cpp
// LLVMBackend.h - next to CreateLfence (~3075)
void CreateFenceAcquire()
{
    builder->CreateFence(llvm::AtomicOrdering::Acquire);
}
```

```cpp
// MainListener.h - next to the __lfence intercept (~14485)
if (functionName == "__atomic_acquire_fence")
{
    Compiler(ctx)->CreateFenceAcquire();
    ...
}
// and add "__atomic_acquire_fence" to the named intrinsic set at ~16958, alongside
// "__lfence" / "__pause" - so it needs no extern stub, same as the rest of intrinsic.cb.
```

(Ordering-first spelling, to match the existing `__atomic_release_store_i64` /
`__atomic_acquire_load_i64`. These are internal stubs, not customer-facing types, so
consistency with the neighbours beats consistency with C++.)

Unlike `__lfence` this is valid on every target: it lowers to `dmb ishld` on arm64 and to *no
instruction at all* on x86 - while still constraining LLVM's optimizer, which is half the
point. It needs no `if const` guard.

The wrapper goes in **`core/intrinsic.cb`, not `atomic.cb`** - that file is already the
hardware layer, and its contract is exactly what this needs: a portable wrapper with the target
gating hidden inside ("unlike the x86-only rdtscp()/lfence()/pause(), they need no __X86__
guard"). Placing `fence_acquire()` two functions from `cycle_count_serialized()` - which uses
`__lfence` for *timing* - puts the two meanings of "fence" side by side where the difference is
visible: one is a memory-ordering barrier, one is an instruction-stream serializer. And the
absence of an `if const (__X86__)` guard becomes a deliberate claim rather than an oversight,
because `pause()` right above it has one.

It needs no `extern` stub. `intrinsic.cb` declares none - `__popcount` / `__prefetch` /
`__lfence` are called bare because they live in the compiler's named intrinsic set
(`MainListener.h:16958`). The `__atomic_*` family needs externs only because it is
prefix-intercepted and the compiler reads the signature off the declaration. Add
`__atomic_acquire_fence` to the **named** set and the wrapper is three lines:

```cflat
// core/intrinsic.cb - next to pause() / cycle_count_serialized()

// Acquire fence: orders every load issued before it against every load and store issued
// after it.  Needed when a validating re-read must not be hoisted above the reads it
// validates - an acquire LOAD only orders forward, so it cannot do this job.
//
// Portable: no __X86__ guard.  Lowers to `dmb ishld` on arm64 and to NO instruction on x86
// (TSO already forbids load-load reordering) while still constraining the optimizer.  This
// is why it is not lfence(): LFENCE is x86-only AND, since Spectre, an execution-serializing
// barrier costing tens of cycles - unaffordable in a hot descent path, and buying an ordering
// guarantee x86 gives away for free.
void fence_acquire()
{
    __atomic_acquire_fence();
}
```

Do NOT hoist the rest of the `__atomic_*` stubs into `intrinsic.cb` or de-duplicate the
redeclarations in `spsc_queue.cb`. The compiler intercepts by name regardless of which file
declares the stub, so the layering is a convention, not an enforcement point - churn without
functional change. The atomic *types* belong in `atomic.cb`; the fence is the only member of
this work with no type attached, which is precisely why it is the one that moves.

Also expose the release store that `atomic__i64.release()` already uses internally, since
`olock.release()` needs it without a lambda:

```cflat
// core/atomic.cb, on atomic__i64
// Store with release ordering: writes issued before it cannot sink below it.
void store_release(i64 val) {
    __atomic_release_store_i64(&_value, val);
}
```

Phase 1 stands on its own merits: `spsc_queue.cb` and `threadpool.cb:153` currently lean on
acquire-*loads* where an explicit fence would state the intent directly.

## Phase 2 - `IOptimisticLockable` and `olock` (core)

**LANDED 2026-07-14.** `core/interfaces.cb` + new `core/hpc/olock.cb`. Ships the RAW api only -
`read(lock(this.optimistic) ...)` waits on Phase 3. `[Capability(ILockable,
IOptimisticLockable)]` compiled with **zero compiler change**, exactly as the multi-arg
precedent in `mutex.cb` predicted. `test.sh` 167/0/15; `test_sync` 65/65.

**The seqlock demonstrably works, and the test is not vacuous.** Under contention (4 readers x
20k optimistic reads racing 20k `lock(ver){ a++; b++; }` writes):

```
total reads  : 80000
validated    : ~53000
INVALIDATED  : ~27000   (~33% - the readers really did race the writer)
torn         : 0        (not one validated read ever saw a != b)
```

That invalidation rate is the load-bearing number. A concurrent seqlock test in which validation
*never* fails proves nothing - it means the readers never overlapped a write and the retry path
was never exercised. `testOlockConcurrentReadersWriter` now asserts `invalidated > 0` explicitly
so it cannot silently rot into a vacuous test. Any future concurrent test in this plan must do
the same.

The original design notes follow.

```cflat
// core/interfaces.cb
// Optimistic (version-validated) read contract.  A reader snapshots the version, reads
// speculatively, then validates; a failed validation means the read is garbage - restart.
// Readers issue no stores, so the read path never dirties the lock's cache line.
interface IOptimisticLockable
{
    // Snapshot the current version, blocking while a writer holds the lock.
    i64  read_begin();
    // True when nothing has changed since read_begin() returned v.
    bool read_validate(i64 v);
    // Promote an optimistic snapshot to an exclusive lock.  False means restart.
    bool upgrade(i64 v);
};
```

```cflat
// core/hpc/olock.cb - version lock (seqlock) for optimistic lock coupling.
//
// Version encoding:
//   bit 0      - obsolete (node retired; any reader holding it must restart)
//   bit 1      - write-locked
//   bits 2..63 - change counter
// release() adds 2, which clears bit 1 and bumps the counter in a single store.

import "interfaces.cb";
import "atomic.cb";
import "intrinsic.cb";   // pause()

i64 OLOCK_OBSOLETE = 1;
i64 OLOCK_LOCKED   = 2;

[Capability(ILockable, IOptimisticLockable)]
struct olock {
    atomic__i64 _v = default;

    // Snapshot the version for an optimistic read.  Spins while a writer holds the lock.
    i64 read_begin() {
        while (true) {
            i64 v = _v.load();
            if ((v & OLOCK_LOCKED) == 0) return v;
            pause();
        }
    }

    // True when no writer has touched the node since read_begin() returned v.
    // The fence stops the caller's field loads from sinking below this re-read; without
    // it the check validates data it has not read yet.
    bool read_validate(i64 v) {
        fence_acquire();
        return _v.load() == v;
    }

    // Optimistic -> exclusive without re-descending.  False means a writer won the race.
    bool upgrade(i64 v) {
        return _v.cas(v, v | OLOCK_LOCKED);
    }

    // Speculative read: snapshot, run body, validate.  False means restart.
    // This is the THIRD bracket - the missing member of the acquire/release family.
    bool read(lock(this.optimistic) Lambda<void()> body) {
        i64 v = read_begin();
        body();
        return read_validate(v);
    }

    // ILockable - exclusive acquire.  Backs `lock (n.ver) { }`.
    void acquire() {
        while (true) {
            i64 v = read_begin();
            if (_v.cas(v, v | OLOCK_LOCKED)) return;
            pause();
        }
    }

    // ILockable - release.  +2 clears OLOCK_LOCKED and bumps the counter, so every
    // optimistic reader that snapshotted the old version now fails validation.
    void release() {
        _v.store_release(_v.load() + 2);
    }

    // Retire the node: readers must restart, and the memory may be reclaimed once the
    // epoch advances.  Caller must hold the lock.
    void obsolete() {
        _v.store_release((_v.load() + 2) | OLOCK_OBSOLETE);
    }

    // True when the node has been retired out from under a snapshot.
    bool is_obsolete(i64 v) {
        return (v & OLOCK_OBSOLETE) != 0;
    }
};
```

**The writer side needs no new syntax.** `release()` ends in a release store, so the existing
scoped statement *is* the seqlock write:

```cflat
lock (n.ver) {          // acquire(): CAS in OLOCK_LOCKED
    n->keys[i] = key;   // guarded fields, exclusive mode
    n->count  += 1;
}                       // release(): release-store v+2, publishing the writes
```

That falls out of `[Capability(ILockable)]` with zero compiler work. The entire cost of OLC is
on the read side.

## Phase 3 - lock modes (compiler)

**LANDED 2026-07-14.** `LockMode` lattice in `LLVMBackend.h`; `currentLockSet` is now
`unordered_map<string, LockMode>`; `.optimistic` is a third mode suffix (**no grammar change** -
it parses as member access on the lock arg, exactly as `.read` / `.write` do). `kCapabilities`
stayed at **four columns**: `IOptimisticLockable` gets no row, and the mode -> interface mapping
it needs lives in `CapabilityForLockMode()` next to the table. Read-vs-write is decided at the
access site by a new `CheckGuardedWrite()`, called on the RESOLVED lvalue of an assignment and
of postfix `++`/`--` - so a guarded field read inside an lvalue (`a[n->count] = x`) is not
mistaken for a write. `olock.read(lock(this.optimistic) Lambda<void()>)` ships in
`core/hpc/olock.cb`. Tests: `Test/errors/err_lock_write_under_read.cb`,
`err_lock_write_under_optimistic.cb`, `err_lock_optimistic_wrong_type.cb`, and two positive
`olock.read()` cases in `Test/test_sync.cb`. `test.sh` 170/0/15.

Known gap, deliberate: the call-site requires-clause check (`CheckCallSiteLocks`) stays
mode-INSENSITIVE (membership only). Calling a function whose lock clause implies exclusive from
inside a `lock(m.read)` block is still accepted. Closing it would demand a mode on every
positional-lock-group method, which would reject read-only group methods called under a shared
lock. No caller exists; revisit with evidence.

The original design notes follow.

`currentLockSet` is a flat `std::unordered_set<std::string>` (`MainListener.h:1959`). The
guarded-field check (`:12766`), the member check (`:16902`), the guarded-global check
(`:18057`), and the function requires-clause check (`:18117`) all test **membership only**.
There is no mode anywhere in it.

Consequence today, with no B-tree in sight: `lock (rw.read) { }` and `lock (rw) { }` confer
identical rights, so **a write to a guarded field while holding only a read lock is not
diagnosed.** That is an existing soundness hole in `ISharedLockable`.

OLC promotes that hole from a bug to a catastrophe. An optimistic reader holds *nothing*. A
write from inside a speculative body is not an ordinary race - it is a write into a node that
may be mid-split or already retired.

The fix is one lattice, and it closes both:

```cpp
enum class LockMode { Exclusive, Shared, Optimistic };
std::unordered_map<std::string, LockMode> currentLockSet;
```

with the guarded-access check learning read-vs-write at the access site:

| Held mode | Read guarded field | Write guarded field |
|---|---|---|
| `Exclusive`  | ok | ok |
| `Shared`     | ok | **error** |
| `Optimistic` | ok | **error** |

New diagnostics:

```
Field 'count' is guarded by 'ver': cannot write it while holding 'n.ver' in read mode.
Field 'count' is guarded by 'ver': cannot write it inside an optimistic read of 'n.ver'.
```

Mode enters through the existing `.read` / `.write` suffix machinery - `.optimistic` is one
more suffix, so this likely needs **no grammar change**. The lambda-parameter seeding site
(`MainListener.h:16223`) learns to seed `Optimistic` when the parameter is declared
`lock(this.optimistic)`.

Note `IOptimisticLockable` does **not** need a `kCapabilities` row. That table exists so
`lock (m) { }` can *lower* to an acquire/release pair. An optimistic read lowers to nothing -
it is an ordinary method call taking a lambda. It is a **checking** capability, not a
**lowering** capability, and the four-column table survives untouched.

## Phase 4 - OLC on the tree: lookup + insert (core), and the gap it exposes

**LANDED 2026-07-14.** `test.sh` 170/0/15; `test_hpc` 236/236; 40 consecutive runs of the
concurrent tests, 0 failures; `leaks --atExit` 0 leaks. Lookup is fully optimistic (zero stores
on the validated path); insert is optimistic-descent + `upgrade()` on the fast path, falling back
to an exclusive lock-coupled preemptive-split descent when the leaf is full. No B-link, no
reclamation. `remove()` / `set()` / `clear()` / cursors remain single-threaded-only.

Measured under contention: **3.5-5.6% of reads invalidated** (~9.5k per run out of ~200k
lookups) plus ~9.3k writer-side restarts. Legitimately lower than `olock`'s 33% because
contention spreads across ~7k leaves; collisions concentrate at the root and internal nodes.

### Three findings worth keeping

**1. The root slot needs its own lock, and this is a real bug, not a formality.** `btree` gained
an `olock _ver` guarding the `_root` pointer, so the *tree itself* acts as the first parent in
the coupling chain. Without it: a reader snapshots `_root`, a concurrent root split replaces it,
the reader descends into what is no longer the root, validates cleanly against that node's
version, and **reports a false miss**. A lost lookup with no torn read and no crash - the
version check on the node cannot see that the node is no longer reachable from the root. Root
creation and height growth must be coupled through exactly like any other child pointer.

**2. Concurrent `insert()` must never overwrite.** It has `add()` semantics deliberately. An
overwrite frees the old value, and freeing anything under optimistic readers is a use-after-free
- the same cliff as Phase 6's merge, arriving from an unexpected direction.

**3. The concurrency test was nearly vacuous on the first attempt.** A fixed reader iteration
count let the writers finish before the readers warmed up: 9 restarts out of ~200k reads
(0.005%). Rewritten with readers running off a stop-flag so they span the entire write window,
the rate went to 3.5-5.6%. **A concurrent test that does not assert a non-zero restart rate is
not a concurrent test.** Both `test_sync.cb` and `test_hpc.cb` now assert it explicitly.

Scope is **lookup and insert only** - concurrent structural `remove` is Phase 6, for reasons set
out there.

The node gains a version lock, and the fields it protects would - if we took Option B below -
move into a guard group:

```cflat
// core/hpc/btree.cb - what Phase 4 adds to the Phase 0 node.
struct btree_node<K, V> {
    olock ver = default;
    lock(ver) {                                    // Option B only; Option A leaves these bare
        int              count        = 0;
        bool             isLeaf       = true;
        K                keys[16]     = default;
        V                values[16]   = default;
        btree_node<K,V>* children[17] = default;
        btree_node<K,V>* next         = nullptr;
    }
};
```

### 4a. What the lambda form gives you

For a read that touches exactly one node, `olock.read()` is complete and checked:

```cflat
// Read a leaf's value under an optimistic snapshot.  No locks, no stores.
bool leaf_get(btree_node* n, i64 key, i64* out) {
    bool found = false;
    i64  v     = 0;
    bool valid = n->ver.read(() => {
        int i = find_slot(n, key);          // guarded fields: legal, read-only
        if (i < n->count && n->keys[i] == key) {
            found = true;
            v     = n->values[i];
        }
    });
    if (!valid) return false;               // torn read - caller restarts
    if (found) *out = v;
    return found;
}
```

The `lock(this.optimistic)` grant lets the body touch `n`'s guarded fields; Phase 3 stops it
writing them. Nothing new is needed.

### 4b. What it does not give you

A descent must **read the child pointer, then validate the parent, then dereference the
child** - the validation point sits between producing a value and using it. The lambda form
cannot express that: the value would have to escape the closure before it has been validated.
So the descent has to be written with the raw calls:

```cflat
// OLC lookup, as it must be written TODAY (Phase 1-3 landed, no new syntax).
bool lookup(i64 key, i64* out) {
    while (true) {                                   // restart loop
        btree_node* n  = _root;
        i64         v  = n->ver.read_begin();
        if (n->ver.is_obsolete(v)) continue;         // root was retired; re-read _root

        while (!n->isLeaf) {                         // <-- guarded field, NO grant here
            int         slot  = find_slot(n, key);
            btree_node* child = n->children[slot];   // <-- guarded field, NO grant here

            i64 cv = child->ver.read_begin();        // snapshot child...
            if (!n->ver.read_validate(v)) break;     // ...then validate PARENT. This is
                                                     // the coupling. Failure -> restart.
            n = child;
            v = cv;
        }
        if (!n->isLeaf) continue;                    // broke out on a failed validate

        return leaf_get(n, key, out);
    }
}
```

**This does not compile**, and the reason is the whole point of the doc. The raw form has no
lambda, so nothing seeds the lock set, so `n->isLeaf` and `n->children[slot]` are guarded
fields accessed with no held guard. The two forms are each half a solution:

- the **lambda form** grants access to guarded fields but cannot let a value cross the
  validation boundary;
- the **raw form** expresses the coupling but forfeits the guard grant.

Neither alone can write a descent. Phase 4 therefore has exactly two ways forward, and we
should pick deliberately:

**Option A - drop the guard group.** Leave `btree_node`'s fields ungrouped, i.e. give up
static checking on the tree's own fields, and write the descent with raw calls. The tree gets
built; the checking that motivated Phase 3 does not apply to the one data structure it was
designed for. Honest, and it produces the evidence for Phase 7.

**Option B - add the statement now.** A region whose failure edge is explicit:

```cflat
// OLC lookup with the proposed `optimistic` statement.
bool lookup(i64 key, i64* out) {
    while (true) {                              // restart loop
        btree_node* n = _root;
        while (true) {
            btree_node* child = nullptr;        // declared OUTSIDE - only trusted if the
            bool        leaf  = false;          // region completes without taking `else`

            optimistic (n.ver) {                // read_begin(); body; read_validate()
                leaf  = n->isLeaf;              // guarded fields: granted, read-only
                child = n->children[find_slot(n, key)];
            } else {
                break;                          // validation failed -> restart descent
            }

            if (leaf) return leaf_get(n, key, out);
            n = child;                          // safe: the region validated
        }
    }
}
```

Values are declared outside the region and assigned inside; the `else` edge is the only way
out on failure, so nothing unvalidated is ever used. That solves the escape problem cleanly.
What it does *not* solve is the restart target: the failure edge belongs at the top of the
whole descent, and `break` reaching it through a doubled `while (true)` is a tell that a
`restart` keyword is the real answer.

**DECIDED (2026-07-14): Option A.** Build the tree unguarded, write the restart loops by hand,
and let the resulting code argue for the syntax. This is the same discipline
`lock-capability-interface.md` applied when it rejected role annotations for having no
statement to consume them. Do not invent `optimistic` / `restart` before there is a caller
that wants them.

The cost is real and should be stated rather than glossed: **the one data structure Phase 3's
lattice was designed to protect is the one data structure that will not be protected by it.**
`btree_node`'s fields stay ungrouped, so nothing stops a speculative body from writing them -
the invariant that makes OLC sound is enforced by review, not by the compiler, for exactly as
long as Option A stands. Phase 3 still pays for itself on `rwlock` (it closes a live hole where
`lock(rw.read)` permits writes to guarded fields), which is why it survives this decision. But
if the tree ships and Phase 7 never happens, we will have built the checker and left the thing
it was for unchecked. That is an acceptable trade only if Phase 7 actually gets revisited once
the tree exists.

### 4c. Insert, and where `upgrade` earns its keep

A writer descends optimistically like a reader, then promotes at the leaf:

```cflat
void insert(i64 key, i64 val) {
    while (true) {                                 // restart loop
        btree_node* n = _root;
        i64         v = n->ver.read_begin();
        // ... optimistic descent to the leaf, exactly as in lookup ...

        if (!n->ver.upgrade(v)) continue;          // optimistic -> exclusive; lost the race
        // n is now exclusively locked, but WITHOUT a lock() block holding it.

        if (n->count < BTREE_FANOUT) {
            leaf_insert(n, key, val);
            n->ver.release();                      // manual release - non-lexical
            return;
        }
        n->ver.release();
        split_and_retry(key, val);                 // rare path: take real locks, restart
    }
}
```

Note what `upgrade()` does to lock lifetimes: it hands you an exclusive lock **acquired
non-lexically**, from inside a region that is not a lock block, which must then be released by
hand. This is the same shape as the hand-over-hand problem in `lock-guard-mandatory.md`, and
it is independent evidence for the same conclusion: **raw `acquire`/`release` must stay legal,
and any scoped sugar is strictly additive.** The abandoned phase 5 of
`lock-capability-interface.md` (making raw `acquire` a compile error - a different doc's
numbering) would break OLC outright. That doc already declined it; this is a second reason.

## Phase 5 - prove it (performance)

**LANDED 2026-07-14.** `performance/perf_btree_scaling.cb`. Measured on Apple M5 Pro (18
logical cores, 6P+12E). **The thesis holds.** olc/coupling ratio at 1 thread (pure per-op
overhead, no contention): 1.38x on both tree sizes. At 18 threads: **119.43x on the small
(1k-key) tree, 14.30x on the large (1M-key) tree** - the ratio grows monotonically with thread
count on both sizes, which is the result the plan said to look for first. Both rwlock modes'
aggregate throughput stayed roughly FLAT (~6-10M ops/s) from 1 thread to 18 threads - zero
scaling despite 18x the hardware - while OLC scaled from 58M to 899M ops/s (small) and 8.9M to
99.9M ops/s (large). Checksums (defeating DCE) matched the independently-computed expected
value on every cell. See the file header for full methodology (zero writers, best-of-3,
deterministic LCG shared across modes).

### The 1 -> 2 thread cliff is the actual result

The ratio at 18 threads is the headline, but the mechanism is proved by a single transition.
Small tree, rwlock-coupling:

```
threads   rwlock-coupling      olc
      1       41,774,584/s     58,159,823/s
      2        9,113,610/s    112,069,931/s     <-- coupling LOST 4.5x by adding a core
```

**Adding a second core made rwlock coupling 4.5x slower in aggregate.** Not "failed to scale" -
*negative* scaling. With zero writers there is no logical contention whatsoever; every reader
could hold every read latch simultaneously and none ever blocks. The only thing that changed is
that a second core started touching the root latch's cache line. At 1 thread that line sits
unchallenged in one core's L1; the moment it is shared, it ping-pongs, and throughput craters.
It then stays flat (~6-9M/s) all the way to 18 threads.

That is the entire thesis of this plan, visible in one row, with every confound removed. It is
also why the 1-thread numbers matter: uncontended, `rwlock-coupling` (41.8M) and `global-rwlock`
(40.7M) are indistinguishable, and OLC's edge is a modest 1.39x of pure per-op cost. **The lock
was never slow. Sharing it was.**

### Honest counter-notes

- **On the large tree at 1 thread, `global-rwlock` (9.9M) BEATS OLC (8.8M).** One lock acquire
  beats seven acquire fences down a depth-7 descent. OLC is not free, and single-threaded it can
  lose. It wins the moment a second core appears.
- The large tree's 18-thread ratio (14.4x) is far below the small tree's (119x), and that is
  expected, not a disappointment: 1M keys spread readers across thousands of leaves, so there is
  less coherence traffic for OLC to eliminate. The concentrated case is the sensitive one.
- Apple's P/E cores make the raw throughput curves non-monotonic in places. This is why the
  ratio, not the curve, is the metric - both modes run the same thread counts on the same
  scheduler in the same run, so the heterogeneity penalty cancels.

The benchmark uses its own local node type (fanout 16, i64 keys/values, carrying both an
`olock` and an `rwlock` per node) - `core/hpc/btree.cb` is not modified. One bug found and
fixed along the way, worth recording: the benchmark's `insertChildAt` helper initially took
`rightChild` without `move`; the ownership system auto-freed the freshly split-off internal
node at `splitChild`'s scope exit (the leaf-split branch masked the same mistake because
`right` escapes earlier via `left->next = right`). `core/hpc/btree.cb`'s real
`_insertChildAt` already has `move` on this parameter - this was a benchmark-local
transcription bug, not a core bug.

Original design notes follow.

`performance/perf_btree_scaling.cb`, run under `performance.bat` with `-O2`, next to
`perf_hashset_dict.cb` (the natural baseline) and `perf_spin_contention.cb`.

This is not a victory lap; it is the experiment. The entire case for OLC over rwlock coupling
is a *scaling* claim. Measure lookup throughput at 1, 2, 4, 8, ... threads for:

1. one global `rwlock` over the tree (the floor),
2. per-node `rwlock` latch coupling (the textbook answer),
3. per-node `olock` optimistic coupling.

If (3) does not pull away from (2) as core count rises, the premise is wrong and none of the
rest of this is worth building. That is the result to look for first.

## Phase 6 - concurrent structural remove: DEFERRED (2026-07-14)

Any scheme where readers hold no lock means **a node can be freed while a reader is inside
it.** OLC is sound only under deferred reclamation - epoch-based (EBR/QSBR), hazard pointers,
or RCU. That remains true. But deferring it is safe, and the reason is worth being precise
about, because it draws a hard line through the feature set:

**Lookup and insert never free a node.** A descent only reads. A split only *allocates* - it
creates a right sibling and adds a separator; nothing is retired. So an optimistic reader can
never hold a pointer to freed memory in an insert/lookup workload, no matter how much
concurrency is thrown at it.

**Only merge frees a node**, and merge only happens on `remove()`. That is the single operation
that hands an optimistic reader a dangling child pointer.

So the line is:

| Concurrent operation | Needs reclamation? |
|---|---|
| optimistic `lookup` | no |
| optimistic descent + `upgrade` + leaf `insert` | no |
| `insert` with node split | no - splits allocate, never free |
| `remove` that tombstones within a leaf (no merge, no free) | no |
| `remove` with borrow/merge (frees a node) | **yes - unsound without it** |

The concurrent tree therefore ships **lookup + insert** first, and structural delete is gated
behind reclamation. If concurrent delete is wanted before then, the escape is
**tombstone-only delete**: mark the entry dead in the leaf, never merge, never free. No node is
ever retired, so no reader can touch freed memory, and version validation already covers the
in-leaf mutation. The cost is space and a leaf population that decays over time until a
single-threaded rebuild compacts it - a trade several real systems take deliberately.

This is not a licence to forget the problem. When it comes back it will be an **ownership**
problem, not a locking one, and it will collide with `IsOwning` / `move` / destructor emission:
a retired node's memory outlives its removal from the tree by an interval set by a global epoch
counter, not by a scope, and `delete` at the point of removal is a use-after-free. An owning
pointer whose destruction is deferred to an epoch is not something the current lifetime model
can express. **It may need a language change.** Scope it on its own, from scratch, when
structural concurrent delete is actually wanted - do not bolt it onto a later phase of this
plan.

Note this does not affect v1 at all: the single-threaded tree owns its nodes normally and
`delete`s them at merge, which is correct because there are no concurrent readers.

## Sequencing

| # | Item | Category | Status / blocking |
|---|---|---|---|
| 0 | `btree<K,V>`, single-threaded, no locks | core (`core/hpc/btree.cb`) | **LANDED** 2026-07-14 |
| 1 | `__atomic_acquire_fence` + `fence_acquire()` in `intrinsic.cb` + `store_release()` on `atomic__i64` | compiler + core | **LANDED** 2026-07-14 - verified `dmb ishld` on arm64 |
| 2 | `IOptimisticLockable`, `olock` | core | **LANDED** 2026-07-14 - ~33% invalidation, 0 torn reads under contention |
| 3 | `LockMode` lattice + `.optimistic` + `olock.read()` | compiler | **LANDED** 2026-07-14 - no grammar change; `kCapabilities` still four columns |
| 4 | OLC on the tree: optimistic **lookup + insert** only | core | **LANDED** 2026-07-14 - 40 consecutive concurrent runs; 3.5-5.6% invalidation |
| 5 | `perf_btree_scaling.cb` | performance | **LANDED** 2026-07-14 - thesis confirmed; olc/coupling ratio 1.38x (1 thread) -> 14.30x/119.43x (18 threads) |
| 6 | concurrent structural `remove` | core + ? | **gated on reclamation - deferred.** Tombstone-only delete is the escape |
| 7 | `optimistic` / `restart` syntax | compiler | **NOT JUSTIFIED - do not build.** Phase 4 produced the evidence; see Phase 7 |

Items 1 and 3 are independently defensible - one fills a real hole in the fence story, the
other fills a real hole in `ISharedLockable` - whether or not the concurrent tree is ever
built. Those are the right things to commit to first.

Item 5 should probably be pulled earlier than its position suggests. The entire case for OLC
over rwlock latch coupling is a scaling claim; if the curves do not diverge, items 4, 6, and 7
are all wasted. Consider measuring rwlock coupling against a stub as soon as item 0 lands.

## Phase 7 - `optimistic` / `restart` syntax - VERDICT: NOT JUSTIFIED (2026-07-14)

**Do not build this.** Phase 4 was sequenced specifically to produce the evidence for or against
it, and the evidence is against. Recording the reasoning so it is not re-litigated.

The proposal was Option B in 4b: an `optimistic (n.ver) { } else { }` region plus a `restart`
keyword. Two things were supposed to justify it. Neither did.

**`restart` would save almost nothing.** The restart loops in the real descent are ~3 lines
each. There is no scaffolding to eliminate.

**The `.optimistic` guard mode would have caught almost none of the real hazards.** Here is what
Option A actually cost - every place the missing check mattered in the shipped descent:

| # | Hazard in the real code | Would `.optimistic` have caught it? |
|---|---|---|
| 1 | speculative body reads `count`/`keys[]`/`children[]` unguarded; a store there would compile | **yes** - this is the one |
| 2 | hand-written index clamps in `_childIndexSpec` / `_leafSlotSpec`; invariant 2 dies silently without them | no |
| 3 | copy-out-then-validate discipline in `lookup()` (`V found` exists solely so nothing unvalidated is used) | no |
| 4 | null-child guard - a torn `count` can run ahead of the store that fills the child slot on arm64 | no |
| 5 | six hand-matched `release()` sites for a lock `upgrade()` acquired non-lexically | no |

Only (1) is a lock-mode question, and the speculative region turned out to be **read-only by
construction** anyway - the discipline was not hard to hold. The hazards that actually had teeth
were (2) and (4), which are *value-range* reasoning about what a torn read can produce. **No
lock-mode lattice can express that.** A mode says "you may not write here"; it cannot say "this
integer you just read may be garbage, so bound it before you index with it."

**If anything argues for compiler help it is (5)** - pairing a non-lexical `release()` across six
exit paths - and the answer there is a scoped guard with an early-exit escape, which is a
different feature that `optimistic`/`restart` does not provide.

This does not retroactively invalidate Phase 3. Its justification was always the live
`ISharedLockable` hole (`lock(rw.read)` permitting writes to guarded fields), it closed that,
and it fixed a nested-lock set-drop bug on the way. But the claim in Option A that "if the tree
ships and Phase 7 never happens, we will have built the checker and left the thing it was for
unchecked" is now answered: the thing it was for did not need it.

## Tests

Per CLAUDE.md, extend existing files rather than adding new ones:

- `Test/test_sync.cb` - `olock` semantics: a validated read under no writer succeeds; a read
  concurrent with a writer fails validation; `upgrade()` succeeds from a clean snapshot and
  fails after an intervening write; `lock (o) { }` works on an `olock` (the `[Capability]`
  path).
- `Test/test_hpc.cb` - the tree under N threads: concurrent lookup/insert, no lost keys.
- `Test/errors/err_*.cb` - new files here are the documented pattern. Two:
  - `err_lock_write_under_read.cb` - writing a guarded field while holding `.read`.
  - `err_lock_write_under_optimistic.cb` - writing a guarded field inside an optimistic body.

## Alternatives rejected

**Build the seqlock on `lfence()`.** x86-only, compiles away on arm64, and it is an
instruction-serializing fence for timing, not a memory-ordering fence for synchronization.
Green on the Mac, corrupt on the Mac.

**Use an acquire *load* for the validating re-read instead of a fence.** Wrong direction.
Acquire ordering keeps later operations from moving earlier; the seqlock needs to keep *earlier
loads from moving later*. This is the bug that produces a one-in-ten-million corrupted lookup
and it is why the kernel puts an `smp_rmb()` before the retry comparison.

**Rely on the `body()` call boundary as the barrier.** Fails twice: closure monomorphization
(`internal/plan/closure-generics-monomorphization.md`) will inline the body away, and a
function call is not a hardware barrier - arm64 reorders loads regardless of the call graph.

**Add `optimistic` / `restart` syntax up front.** No caller exists yet. Build the tree first
and let it argue.

**rwlock latch coupling and skip OLC entirely.** Defensible if Phase 5 shows no divergence.
That is precisely what Phase 5 is for - but it should be measured, not assumed.
