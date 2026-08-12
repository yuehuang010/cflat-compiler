# q11: Global, program-lifetime, and static storage

3 active items remain. The independent static-local origin/DWARF item is fixed in Q11. The
remaining ownership questions stay blocked on the lifetime ruling below. Ownership machinery is
written against `AllocaInst` locals. Storage that is not a local -
a `GlobalVariable`, program-lifetime storage, a static local - falls out of the analysis or is
re-entered a second time in a state the analysis never anticipated.

## Shared root cause

Storage-class blindness. Guards, taint recording, and codegen tails test for or assume an alloca,
so global and static destinations skip the path entirely; and nothing re-initializes
program-lifetime storage between entries, so any "consume once" rule silently becomes "works the
first run, breaks the second".

## Members

- `p2/move-out-of-program-lifetime-storage-crashes-on-reuse` - storage is not re-initialized
  between calls, so a second `move` double-frees the zeroed value.
- `p3/implicit-consume-of-a-global-owner-loses-the-value-on-the-second-run` - assignment from a
  global owner now takes the transfer arm, nulling the global on first use.
- `p2/global-pointer-destination-does-not-propagate-borrow-taint` - taint recording is gated on
  `AllocaInst` storage, excluding `GlobalVariable` destinations.
- Fixed in Q11: `p3/static-local-missing-origin-slot-and-dwarf` - static-local codegen now gets a
  module-lifetime origin slot and local-to-unit DWARF global entry.

## SPIKE 2026-08-11: measured behaviour (Release, macOS arm64, `scratch/uniqglobal/`)

Run BEFORE ruling, because the "globals are never destructed" premise turned out to be FALSE.
Today's behaviour is split along storage type:

| storage | destructed at exit? | implicit consume | explicit `move` |
|---------|---------------------|------------------|-----------------|
| `unique T*` global | NO | REJECTED, with the borrowed-value diagnostic | works; nulls the global; the local destructs at scope exit |
| `static unique T*` local | NO | - | - |
| struct-with-destructor global | YES, reverse declaration order | SILENTLY TRANSFERS (the bug) | compiles; the global's destructor still runs on the moved-from value |

So the `unique` arm is ALREADY the Rust model in full and needs no work. The struct arm is the
C++ model, and its implicit consume (`Res local = gres;` leaves the global reading back null, no
diagnostic) is `p3/implicit-consume-of-a-global-owner-loses-the-value-on-the-second-run`, confirmed
live.

### The C++ destruction-order fiasco does NOT apply to CFlat

Attempted and could not be constructed:

- Within a file a global cannot forward-reference a later global (`~User()` naming a `mutex`
  declared below it fails: `Undefined variable glock`). A dependency is therefore always declared
  EARLIER.
- Destruction is reverse declaration order, and that ordering HOLDS ACROSS IMPORTS - measured: the
  importer's global destructs first, the imported library's globals destruct after.

Dependencies always outlive their dependents, by construction. C++'s fiasco exists because
cross-TU order is unspecified; CFlat pins it and the no-forward-reference rule makes the pinned
order the correct one. **The main argument for the Rust never-destruct model does not apply here.**
Do not cite the fiasco as a reason to stop destructing struct globals.

### Does a global `mutex` expect its destructor? No - but do not remove it

- `~mutex()` calls `os.mutex_destroy` (`cflat/core/os.cb:675`): on Windows a literal no-op
  ("SRWLOCK needs no cleanup"); on POSIX it frees the lazily-`calloc`'d `pthread_mutex_t`, nulls
  the slot, and is idempotent.
- `_srw`'s `unique` is COPY SUPPRESSION ONLY (`cflat/core/mutex.cb:30`), not an allocation to manage.
- For the three process-lifetime registry locks (`_ar_reg_lock`, `_ba_reg_lock`, `_g_numaRegLock`)
  the exit-time run frees 64 bytes just before the process dies - not load-bearing.
- The destructor IS load-bearing for an EMBEDDED/scoped `mutex` (a field in a struct that dies
  during the run), so it cannot simply be deleted.

Residual, unmeasured: a thread still alive at exit that touches a destroyed global mutex would hit
`ensure_mutex` on the nulled slot and CAS-publish a FRESH lock, putting two threads on different
mutexes. That is thread-lifetime, not declaration order.

## RULING 2026-08-11 (maintainer): the Rust model, both arms. UNBLOCKED.

Ruled AFTER the spike above, with the corrected facts in hand. The maintainer chose the Rust model
for the destruction question and for the owning-globals question, in full knowledge that CFlat's
declaration order makes the C++ model safe here. Consistency across the two storage arms was
chosen over keeping today's split behaviour.


The blocking question - what a global owner's consume and destructor semantics are - is answered.
Option 1 of the three that were on the table, plus an explicit never-destruct answer:

1. **Owning types remain LEGAL at global and static scope**, both `unique T*` and a struct with
   owning fields or a destructor. An earlier answer in this session rejected them; it was withdrawn
   once the spike showed the `unique` arm already behaves correctly. Rejecting it would remove a
   checked, well-diagnosed feature and push users onto a bare `T*` global with no checking at all.
2. **Implicit consume from a global or static owner is an ERROR.** Reading such a storage in a
   position that would transfer ownership is rejected with a diagnostic that names the remedy. The
   `unique` arm already does exactly this; the STRUCT arm is the one that must change, and its
   silent transfer is the one confirmed bug in this bucket.
3. **Explicit `move g` is legal and RE-INITIALIZES the storage.** That is the sanctioned spelling
   (the `Option::take` analog), and the re-initialization is what makes reuse - a second entry, a
   second run, and the exit-time state - defined rather than a moved-from husk.
4. **A global/static owner is NEVER destructed.** No exit-time teardown is synthesized, for either
   arm. This is the point that CHANGES behaviour: struct globals are destructed today, in reverse
   declaration order, and that stops. Chosen for consistency with the `unique` arm rather than
   because the current ordering is unsafe - per the spike, it is not.
5. **Program-lifetime storage is the exception, and follows the `thread_local` analog**: its
   teardown point IS provable, so it re-initializes (and destructs) per entry.

### Consequence to land deliberately (point 4)

`core/` loses exit-time destruction of `mutex _ar_reg_lock` (`arena_allocator.cb:185`),
`mutex _ba_reg_lock` (`bucket_allocator.cb:111`), `mutex _g_numaRegLock` (`numa.cb:41`), and
`page_pool g_page_pool` (`page_pool.cb:165`). Per the spike this costs nothing real: `~mutex()` is
a no-op on Windows and frees 64 bytes immediately before process death on POSIX. It also REMOVES
the residual hazard noted above, where a thread still alive at exit touches a destroyed global
mutex and CAS-publishes a fresh one. Embedded and scoped `mutex` values are untouched - they keep
their destructor, which is where it was always load-bearing.

### Prior art this was decided against

- **Rust.** Statics are never dropped - an explicit language decision, because exit order is not
  provable; `lazy_static` / `OnceLock` / `LazyLock` inherit it. Moving out of a static is rejected
  outright ("cannot move out of static item"); the sanctioned way to take an owned value out is
  `Mutex<Option<T>>` + `.take()`, which by construction leaves a defined state. `thread_local!` is
  the one thing that DOES drop, at thread exit, because that lifetime is provable. Points 1, 2, 3
  and 4 above are that model.
- **C++.** Static-duration objects do run destructors (`__cxa_atexit`, reverse construction order,
  unspecified across TUs). In practice this is the static destruction order fiasco: a destructor
  runs while other threads still use the object, or after a dependency is dead. The Google style
  guide bans static objects with non-trivial destructors, and the standard workarounds
  (construct-on-first-use, `new`-and-never-`delete`) all amount to deliberately leaking to avoid
  the destructor. On the consume side C++ offers nothing: `std::move(g)` silently leaves a global
  valid-but-unspecified.

Both converge from opposite directions on the same practical answer, and it matches the recorded
maintainer position that a global's lifetime cannot be proven so a destructor cannot be run for it.
Do not re-open this to propose exit-time destruction of globals.

Note `p3/static-local-missing-origin-slot-and-dwarf` is NOT blocked on this: it is a plain codegen
early-return with no semantic content, and can be fixed independently at any time.

## Fix direction (authorized by the ruling above)

1. Audit for `isa<AllocaInst>` / alloca-shaped assumptions in the ownership paths and replace with
   a storage-class query that admits globals and statics. `p2/global-pointer-destination-does-not-propagate-borrow-taint` and
   `p3/static-local-missing-origin-slot-and-dwarf` are both instances of an early return on that test.
2. Apply the ruled consume semantics uniformly: the two consume issues are the same question asked
   of a global and of program-lifetime storage and MUST get the same answer - implicit consume is
   an error, explicit `move` is legal and re-initializes the storage.
3. `p2/move-out-of-program-lifetime-storage-crashes-on-reuse` needs re-initialization on entry, not
   just a guard.

Reasonably self-contained; can run alongside the q01 chain if the consume-arm edits are kept out
of the shared assignment code that q05 will rewrite. If they are not, sequence after q05.
