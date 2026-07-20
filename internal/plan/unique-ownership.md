# Unique ownership: fields, locals, params, containers, and interface values

Consolidated 2026-07-19 from two plans, and again 2026-07-20 from a third. All now deleted:

- `field-ownership-unique.md` (2026-07-16) - Part I below: the `unique` FIELD qualifier.
  Stages 1-5 DONE, including the code-review remediation and the interface field contract.
- `interface-value-ownership.md` (2026-07-18) - Part II below: interface value ownership and
  `unique` in GENERIC-ARGUMENT position (`list<unique X*>`, `list<unique IShape>`), plus
  unique locals and synthesized move params. Stages 1-6 DONE; Stage 7 (D12) was attempted,
  rolled back 2026-07-19, and superseded. Its dead-end record was deleted 2026-07-20 - the
  final design has landed and D12 must not be revived.
- `unique-field-migration-survey.md` (2026-07-20) - the census that CANCELLED the core field
  migration. Its durable findings are inlined into NEXT item 1 and "Rules that still bind";
  nothing else in it was load-bearing. Deleted 2026-07-20 as part of this consolidation.

**This is the single plan for the `unique` workstream. Do not start a second one.**

Parts I and II were COLLAPSED TO THEIR SETTLED DECISIONS on 2026-07-20, and the dead-end and
cancelled-proposal records deleted, now that the final design has landed. The stage-by-stage
narrative, design rationale, prior-art surveys and code-review logs are all in git history.
Corrections to WRONG diagnoses are kept - they stop a fixed bug being re-derived.

## Status and remaining work (the live section)

Last updated 2026-07-20. Everything above the "Completed" ledger is work that is still open;
the ledger is a one-line-per-item record of what landed. The detailed blow-by-blow of each
completed migration was removed on 2026-07-20 to keep this section readable - it is preserved
in git history, and the durable LESSONS from it are hoisted into "Rules that still bind" below.

### RESUMING FROM A FRESH CONTEXT - read this first

Written 2026-07-20 at the end of a long working session, so the next context can pick up
without re-deriving anything. Delete this subsection once its contents are stale.

**Where the tree stands.** Last commit is `4cce536`. Everything through the six-type RAII
migration is now COMMITTED (`54d6803` = the six container migrations; `4cce536` = the RAII work
and the rest of the 2026-07-20 batch). The ledger rows below carry the real commit; rows still
marked "uncommitted" are genuinely uncommitted working-tree edits.

Uncommitted in the working tree, verified green, no agents in flight:

- `dictionary<K, unique V*>.add()` duplicate-key leak FIXED (`cflat/core/dictionary.cb`), with a
  regression leg in `Test/test_collection_leaks.cb`. Its issue file is deleted in the same edit.
- Synthesized destructor's SCALAR arm widened to `IsUnique || IsUniqueTypeArg`
  (`cflat/LLVMBackend.h` ~3079), plus two regression legs. See NEXT item 3.

**A commit was made in error on 2026-07-20 and undone with `git reset --soft`.** An implementation
agent committed and fast-forwarded onto `master` despite the standing "do not commit" rule. The
change survives as staged working-tree edits. If a future session finds staged-but-uncommitted
`unique` work with no explanation, this is why - it is not an abandoned experiment.

**Verified baselines to measure against:** `bash test.sh Release` = 448 passed, 0 failed,
8 skipped. `bash example_mac.sh` = 35 passed, 0 failed. `bash test_lsp.sh` = 152 passed, 0 failed
(449 faded hints). macOS host - do NOT add "needs Windows verification" caveats; the maintainer
owns that and is aware.

**Standing instructions from the maintainer.** Do NOT commit (stash is allowed); keep `master`
linear, no merge commits. Use the repo-root `scratch/` for ALL temp files. Never use the `haiku`
agent tier. Do not create new `Test/*.cb` files (`Test/errors/err_*.cb` IS sanctioned). Do not
revert changes to check a baseline - ask first. Do not weaken test assertions to make something
pass. The maintainer's standing steer this session was "continue until blocked" and "do not let
commit block you" - so keep batches logically separable for slice-committing rather than idling
on commit boundaries.

**THE THREE PENDING DECISIONS WERE ANSWERED 2026-07-20** - see NEXT items 3 and 4 for the
full rulings. In short: (1) the synthesized destructor will release `unique` fixed-array fields
element by element, which is BREAKING for `btree` and must migrate it in the same change
- LANDED 2026-07-20, see the ledger;
(2) core's manual-lifecycle types go RAII, with copy suppression handled by AUDIT-AND-DISCIPLINE
for now - extending `unique` to non-`delete` releases is the preferred long-term answer but was
explicitly NOT chosen yet - LANDED 2026-07-20 for six types, see the ledger;
(3) `~Thread()` on a still-running thread is an ERROR (compile-time
ideal, runtime acceptable), never a silent join or detach - NOT IMPLEMENTED: the audit found a
real copy path AND a deliberate detach pattern in `channel.operator>>` that the ruling's own
semantics would abort. See `internal/issue/thread-cannot-go-raii.md`.

**Process lessons from this session - these cost real time, do not relearn them.**

- **VERIFY EVERY AGENT CLAIM INDEPENDENTLY.** Across ~8 agents, THREE filed root causes turned
  out to be wrong and were only caught by re-running the repro by hand. The corrected diagnoses
  are recorded in "Resolved 2026-07-20" below and are worth more than the fixes themselves. An
  agent reporting green is not evidence; re-run the suite and the specific repro yourself.
- **Agents that push back are usually right.** Four agents this session contradicted part of
  their brief (the field-migration survey found ZERO of a supposed 34 targets; another proved a
  filed root cause wrong; another refused a "fix" that would have broken shipping code). Every
  one of them was correct. Write briefs that invite this explicitly.
- **Do NOT run two agents in parallel when they share an acceptance gate.** Disjoint SOURCE
  files are not disjoint when both edit the same test file or both gate on the full suite. This
  polluted one agent's gate and produced a phantom failure it correctly refused to own.
- **Split multi-bug issue files.** Three findings once lived in one file whose own instructions
  said "delete when fixed" - fixing the titular bug would have silently discarded the other two.
- **A counting oracle cannot distinguish "both correct" from "both inverted."** This hid a fully
  inverted move-overload pair for hours. Assert identity or an observable effect, never counts
  alone.

**What is genuinely finished.** All six containers (`list`, `hashset`, `dictionary`, `btree`,
`array`, `queue`/`stack`) are on ONE rule. The field migration is CANCELLED with evidence, not
deferred. `alias`-as-type-argument is removed. What remains in NEXT is either blocked on a
decision above or is the low-priority optimisation in item 5.

### THE RULE (settled, and now uniform across core)

**BARE BORROWS, `unique` OWNS - pointers and interface fat pointers alike.**

| Spelling | Meaning |
|---|---|
| `C<T>` (value) | element is COPIED on insert; `move` at the call site transfers instead |
| `C<T*>` | BORROWED - the container never frees it, the caller owns it |
| `C<IShape>` | BORROWED - an interface value is a fat pointer and follows the pointer rule |
| `C<unique T*>` | OWNED - freed on overwrite, on removal, and at teardown |
| `C<unique IShape>` | OWNED - same, through the vtable dtor slot |

Keys in keyed containers stay BORROWED (`alias K`): a key is read for comparison on every
lookup, and an owning key would be destructively moved out by that read.

The goal this serves, in the maintainer's words: *"The more important goal is to align usage of
ptr and fatptr."* Ownership reads off the type at the local declaration site, with nothing at
the call sites - C#-like local readability, with the complexity paid once in the library.

### NEXT - in priority order

1. **Core FIELD migration - CANCELLED 2026-07-20. There are ZERO migratable fields, and
   `unique` FIELDS are an APPLICATION-LEVEL feature, not a core-library one.** A field is
   migratable only if a hand-written destructor frees it with a plain scalar `delete field;`;
   core has none. Every owning pointer field in core releases some OTHER way - `free()`,
   `delete[]`, an OS or allocator call, a refcount, or a chain walk - and `unique` expresses
   none of those. The old "~34 verified-owning fields" figure came from a looser criterion.
   Confirmed by measurement, not just census: the scalar-substitution fix (item 3) moved
   nothing in 635 tests, because core has no field of that shape and structurally wants none.
   **Do not revive this as a sweep, and stop looking for core consumers.**

2. **Remove `alias` as a generic type ARGUMENT - DONE 2026-07-20.** See the ledger row.

3. **`unique` under generic substitution - DONE 2026-07-20 for arrays AND scalars; ONE gap left.**
   The synthesized destructor now releases a `unique` fixed-array FIELD element by element, and
   (second pass, same day) a `unique` SCALAR field reached through substitution.

   **The scalar half, and the maintainer's ruling that drove it.** Ruling: *"`unique` is a mimic
   of C++ `unique_ptr`, so the default destructor should enumerate all fields for `IsUnique` and
   delete them, null-checked."* Executed by widening the scalar arm in `GetOrCreateFullDestructor`
   (`LLVMBackend.h` ~3079) from `f.IsUnique` to `(f.IsUnique || f.IsUniqueTypeArg)`, matching the
   array arm below it. Before: `struct Holder<T> { T _v; }` as `Holder<unique C*>` was released by
   NOTHING - measured `dtor=0 leaks=1`. After: `dtor=1 leaks=0`.

   **Blast radius was zero, and that is a weaker result than it looks.** All three suites were
   identical to baseline (448/0/8, 35/0, 152/0) because NO core type has a scalar
   substituted-`unique` field - core stores values in node/bucket ARRAY fields, already covered by
   the array arm. So the suite proved no regression but could not prove the new arm works. Both
   directions are now pinned by `Test/test_collection_leaks.cb` (`LeakGenSlot<T>`): the `unique`
   instantiation must free exactly once, and the NON-unique instantiation (`LeakGenSlot<PtrLeak*>`)
   must free NOTHING - the second leg is the important one, since freeing borrows was the whole
   risk of widening the predicate.

   **No double-free in core, for a reason worth keeping.** Hand-written teardown frees via
   `_freeValue(move ...)`, and explicit `move` NULLS the source; the synthesized delete is
   null-checked, so it degrades to a no-op. This is not luck - the wrapper runs the user dtor
   FIRST precisely so hand-written free-and-null logic wins (see "Rules that still bind").

   **The scalar `unique IFace` gap - CLOSED 2026-07-20.** The widened guard required `f.Pointer`,
   and a boxed interface value is a `{i8*,i8*}` fat pointer, so `Holder<unique IShape>` measured
   `dtor=0 leaks=1`. Closed by giving the scalar arm its own interface branch
   (`f.IsInterface && !f.IsInterfacePointer`) routing to `EmitUniqueInterfaceFieldRelease`, which
   retargets the builder at the wrapper body and reuses `EmitOwningInterfaceCleanup` (vtable dtor
   slot + `operator delete`) - the same release the array arm gives each element. Both spellings
   now measure `dtor=1 leaks=0`. With the destructor able to express it, `ValidateUniqueField`
   was relaxed for the interface-VALUE case, so a WRITTEN `unique IShape x;` field is now legal
   in both scalar and fixed-array shape. `Test/errors/err_unique_fixed_array.cb` swapped its
   now-stale interface rejection for a still-valid double-indirection one. Issue file deleted.

4. **Teardown gaps in core - DONE 2026-07-20, with two documented SKIPS.** The RAII ruling
   below was executed. Six types went RAII, two were skipped with filed evidence. See the
   ledger row and the "RAII migration outcome" table immediately after this item.

   Original item text kept below for the reasoning trail.

   **Teardown gaps in core, found 2026-07-20 by the field-migration survey.** These are the
   survey's real yield and are INDEPENDENT of the ownership workstream. All share one shape:
   core types with a MANUAL lifecycle (`init` / `start` / `destroy`) and no destructor, where
   the manual call is easy to miss or to double up. **Worth treating as ONE design question -
   should these types be RAII or stay manual - rather than four separate patches.**

   - `internal/issue/rwlock-os-state-never-destroyed.md` - `rwlock.destroy()` has ZERO callers
     tree-wide; every POSIX instance leaks its pthread state. Invisible on Windows (inline
     SRWLOCK), which is why it was never noticed. Independently re-verified.
   - `internal/issue/threadpool-continuation-ctx-leak-on-drop.md` - `threadpool.cb:861-871`
     drops `cont.ctx` on the queue-saturation path. The most concrete leak found.
   - `internal/issue/unguarded-double-init-leaks.md` - `stream.init()` and `Thread.start()` have
     no re-entry guard. `Thread.start()` twice also ABANDONS a running thread that can then
     never be joined - a correctness bug, not just a leak.
   - Not filed separately, recorded in the survey: `block_pool` / `arena_channel` / `page_pool`
     have no destructors and require manual `destroy()`; `event.destroy()` lacks the null check
     its `semaphore.destroy()` counterpart has; `ui_native/win32.cb:509` `Window.tooltip` has no
     teardown call (Windows-only, unverifiable from macOS).

   NOT bugs, recorded so they are not "rediscovered": `NumaThread._pkt` leaks by design on POSIX
   detach/kill; `stop_token._state` is a deliberate shared alias that any shape-based scan would
   misclassify as owning - migrating it would be actively wrong.

   **RULED 2026-07-20 - go RAII, matching the C++ model.** Add destructors; make `destroy()`
   idempotent (null the slot, null-check in the destructor) so the one existing manual call
   (`numa.cb:261`) does not become a double release.

   **The copy-suppression problem, and the interim answer.** C++ RAII is safe because these
   types are non-copyable by the type system (`std::mutex` deletes copy AND move). cflat's only
   non-copyability mechanism is `unique`, which requires a single-indirection pointer and emits
   `delete` - it cannot express `void* _lock` released by `os.rwlock_destroy()`. Maintainer's
   ruling: the RIGHT long-term answer is extending `unique` to cover non-`delete` releases
   (giving teardown and copy suppression from one mechanism), **but that choice is NOT being
   made yet.** For now use DISCIPLINE: audit each type for any copy path (passed by value,
   stored in a copying container, returned by value); if none exists, add the destructor and
   document that the type must not be copied. **If a copy path DOES exist for a type, do not add
   its destructor - report it instead.** A destructor on a copyable value struct double-destroys
   one OS resource per copy.

   **RULED 2026-07-20 - `~Thread()` with a still-running thread is an ERROR, not a guess.**
   Compile-time rejection is ideal; a runtime error is the acceptable fallback. Do NOT silently
   join (a blocking destructor can deadlock at scope exit) and do NOT silently detach (the
   thread outlives its owner and can outlive data it references). This mirrors C++ calling
   `std::terminate()` when a joinable `std::thread` is destroyed.

   **OUTCOME 2026-07-20 - the RAII migration, with its copy-path audit.** Executed on macOS;
   `test.sh Release` 448/0/8 and `example_mac.sh` 35/0 both green after.

   | Type | Copy path found | Verdict |
   |---|---|---|
   | `mutex` | none | **RAII** - `~mutex()` |
   | `rwlock` | none | **RAII** - `~rwlock()` |
   | `condvar` | none (zero uses tree-wide) | **RAII** - `~condvar()` |
   | `event` | none (only `latch._ev`; `latch` never copied) | **RAII** - `~event()` + null guard in `destroy()` |
   | `semaphore` | none (only `ThreadPool._ready` + 3 locals) | **RAII** - `~semaphore()` |
   | `barrier` | none (2 locals; workers get `barrier*`) | **RAII** - `~barrier()` |
   | `Thread` | **YES** - `nw[i] = _workers[i]` in `ThreadPool.resize()`, `threadpool.cb:659` | **SKIPPED** - `internal/issue/thread-cannot-go-raii.md` |
   | `block_pool` / `arena_channel` / `page_pool` | n/a - blocked on a precondition, not a copy | **SKIPPED** - `internal/issue/pools-no-destructor-shutdown-ordering.md` |

   Transitive containment was audited too (`event` -> `latch`; `mutex` -> `BucketAllocator`
   -> `block_pool` -> `arena_channel`; `mutex` -> `barrier`/`stream`/`ThreadPool`/
   `ArenaAllocator`/`NumaDomain`). No copy at any level. Interface-typed use of the two
   allocators is not a copy - an interface value is a fat pointer.

   **Idempotency came free.** `os.mutex_destroy` / `rwlock_destroy` / `cond_destroy`
   (`os.cb:675-739`) already null-check AND null the slot, and `event.destroy()` /
   `semaphore.destroy()` already null their handle. So `numa.cb:261`'s explicit
   `_lock.destroy()`, `barrier.destroy()`'s `_mtx.destroy()`, and `stream`'s `~stream()`
   are all no-ops by the time the member destructor runs. No double release.

   **A safety property worth knowing: destroying a lock cannot strand a caller.**
   `os.mutex_lock` / `rwlock_*_lock` go through `ensure_mutex` / `ensure_rwlock`, which
   lazily RE-CREATE the OS object when the slot is null. So a use-after-destroy on a lock
   silently re-allocates rather than being UB. This materially de-risks the whole change -
   including the shutdown-ordering question for the registry-lock globals
   (`_ba_reg_lock`, `_ar_reg_lock`, `_g_numaRegLock`). Pinned by
   `testLockUsableAfterDestroy` in `Test/test_sync.cb`.

   **`~Thread()` is the one place the ruling itself did not survive contact.** Besides the
   copy path, core DELIBERATELY detaches a running thread from a scope-local `Thread` in
   `channel<T>.operator>>` (`channel.cb:349-350`) - every `a >> b` pipe. An
   error-on-still-running destructor aborts that path; verified experimentally (suite went
   447/1/8, isolated to `testChannelPipeSingle`). So `Thread` needs an explicit `detach()`
   in addition to copy suppression before it can be RAII. The double-`start()` guard from
   the same issue DID land.

   **Oracles used, and where HeapAudit was vacuous.** HeapAudit tracks `new` only, and every
   lock's OS state is `calloc`'d inside `os.posix` - so HeapAudit is **VACUOUS for all six
   RAII types** and was not used as evidence for them. Instead: peak-RSS over 800k
   construct/lock/scope-exit cycles (`scratch/lock_raii_probe.cb`), 1.5 MB with RAII vs
   43.8 MB for a negative control that skips the release - i.e. the oracle was proven
   non-vacuous, and 200k x 200-byte Darwin `pthread_rwlock_t` is exactly the observed gap.
   Double-release would abort under the macOS allocator, so "returns normally" is the
   idempotency assertion. For the threadpool continuation ctx HeapAudit IS valid (the ctx is
   `new`ed by the caller), but the stronger oracle is a DISTINCT per-path dtor whose counter
   identifies WHICH release ran - not a bare count.

5. **Optimisation, not a leak:** `list<string>` named-lvalue sites still deep-copy where the
   source is provably dead. Adding `move` at those call sites is a pure win with no semantic
   change. Ruled an optimisation 2026-07-19 (`live = 0` verified), so it is not urgent.

### Resolved 2026-07-20 - kept for the corrected diagnoses

**The `unique IFace` cluster (2026-07-20).** The one-bug hypothesis was WRONG: three
symptoms, three distinct root causes, two fixed. Kept because each diagnosis corrects a
filed claim that would otherwise be re-derived.

   - *`unique IFace[N]` local frees nothing at scope exit* - FIXED. Not interface-specific at
     all: `unique C*[4]` leaked identically. `EmitDestructorsForScope` had no fixed-array arm,
     so an owning array local was never walked. Added `IsOwningUniqueArray` /
     `EmitOwningUniqueArrayCleanup` (`LLVMBackend.h`), which GEPs each slot and reuses the
     scalar emitters. `IsOwning` is deliberately NOT required there - it is set from a scalar's
     single `new` source and an array has none, so `unique` on the declaration is the ownership
     statement. Moving an interface ELEMENT out now zeroes its `{i8*,i8*}` slot
     (`MainListener.h`, in the `move` arg path) or the teardown double-frees it.
   - *`t.lookup(k, &s)` on `btree<K, unique IFace>` compiles then segfaults* - FIXED, but the
     filed diagnosis was wrong. The moved-variable check fires correctly for BOTH spellings
     (verified at a6f21f4 with the btree guard off); it was never blind to fat pointers. The
     real cause: generic substitution set `IsUniqueTypeArg` on `V* out`, so a POINTER TO the
     owning location read as a `unique` SINK, and `ApplyMoveParamTransfer` nulled the caller's
     variable - the out-param then dangled. Fixed by clearing `IsUniqueTypeArg` when the
     declarator carries an explicit star (`MainListener.h`, after `hasExplicitPointer`).
     `Test/errors/err_moved_out_param_unique_{ptr,iface}.cb` pin both spellings as a pair.
   - *field-shape rule skipped under substitution* - RESOLVED 2026-07-20, and BOTH the filed
     diagnosis and the filed fix direction were wrong. The framing "the check is skipped" is
     itself wrong: `ValidateUniqueField` (`MainListener.h:6605`) is a guard on ONE consumer -
     the synthesized destructor - and under scalar substitution that consumer was not running,
     so there was nothing for the check to guard. Re-validating at monomorphization (the filed
     direction) would have rejected `btree<K, unique C*>` and `btree<K, unique IFace>`, both
     hand-written and correct. The real defect was the opposite of a missing rejection: a
     SILENT LEAK. Fixed by making the consumer run - see NEXT item 3. The referenced issue file
     `unique-field-check-skipped-on-substituted-generic-type.md` never existed on disk; the gap
     lived only in this plan's prose. Do not go looking for it.

   **A third, unrelated bug found by the same repro and fixed with it:** `IsOwningInterfaceValue`
   tested only `TypeAndValue.IsUnique`, which substitution never sets (it records
   `IsUniqueTypeArg`). So `_freeValue(move V value)` with `V = unique IFace` - an unconsumed
   owning move param relying on scope-exit teardown - freed nothing. This is why
   `btree<K, unique IFace>` leaked every value on remove AND teardown, and it is the one place
   where the fat-pointer blind spot the cluster hypothesised actually existed.

   `_placeValue`'s `compile_error` guard and `Test/errors/err_btree_unique_interface_value.cb`
   are deleted; `Test/test_collection_leaks.cb` gains a `btree<int, unique IShapeLeak>` leg
   (400 inserts, multi-level splits, removes forcing merges) plus unique-array-local legs.


**`queue` / `stack` borrow migration (2026-07-20).** The design landed as written: a member-scope
`if const` can wrap a whole method declaration, including one whose RETURN KIND varies
(`move T dequeue()` for owned, `alias T dequeue()` for borrowed, `move T` for value). Both files
were self-inconsistent before - `enqueue(move T)` claimed ownership of a pointer element while
the destructor's `!is_pointer(T)` guard never freed it, so a pointer queue leaked with no API to
prevent it (measured `LEAKS=4` at HEAD, `LEAKS=0` after). No caller in the tree used a pointer
element, so nothing inverted. A pre-existing BUFFER OVERFLOW was found and fixed with it:
`_grow()` was gated on `_size >= _capacity` while writing to `_data[_front + _size]`, so a queue
drained from the front wrote past the end (capacity 4, enqueue 4, dequeue 2, enqueue 1 -> writes
`_data[4]`). `_grow` now tests `_front + _size >= _capacity` and repacks in place when the live
count does not need doubling.

### Rules that still bind (hoisted from the completed migrations - do not re-derive)

- **A user-written destructor does NOT suppress synthesized field teardown - they COMPOSE.**
  `GetOrCreateFullDestructor` emits a wrapper that calls the user dtor FIRST, then each field's
  release (`LLVMBackend.h` ~3123). That ordering exists so hand-written free-and-null logic runs
  before the null-checked synthesized delete, which then no-ops. Every safe interaction between
  core's hand-written teardown and the `unique` field arms rests on this. Do not "optimise" the
  ordering, and do not assume a hand-written `~T()` opts a type out of field teardown.
- **The synthesized destructor covers FIELD-SHAPED ownership only; it can never replace core's
  `is_unique(T)` release code.** It releases a scalar `unique T*` (one pointee) or `unique T* f[N]`
  (N known at compile time). Core's containers own HEAP BUFFERS OF RUNTIME LENGTH - `~list()`
  walks `_size` elements out of `_data` - and no field-shape rule can express that. Of the 32
  `is_unique()` sites in core, most are not in a destructor at all (`_placeAt` decides move vs
  copy; `_releaseAt`, `set`, `remove`, `clear` and `add`'s duplicate refusal all run mid-life,
  while the container is very much alive). Asked directly whether the widened destructor makes
  them redundant: it does not, and removing any of them reintroduces a leak.
- **`&&` and `||` do NOT const-fold** (`internal/issue/if-const-no-constant-folding-path.md`).
  Every compile-time condition MUST be a chained `else if const`. This is why `is_interface(T)`
  had to exist at all - "pointer AND NOT interface" cannot otherwise be spelled.
- **Declare the PLAIN overload BEFORE the `move` one.** Interface conformance matches the FIRST
  declared overload (`internal/issue/interface-conformance-matches-first-overload.md`), and the
  diagnostic on failure is confident and points at the wrong fix.
- **The `if const (!is_pointer(T))` guard on a transfer overload is DESIGN, not a workaround.**
  A borrowing container must not accept ownership of a pointer element - nothing would ever free
  it. For `unique T*` the plain parameter is already a synthesized move sink (D4).
- **There is no `is_alias(T)` intrinsic.** It was removed with the `alias` type ARGUMENT on
  2026-07-20 (the only thing it could ever observe was an alias-qualified type arg). The rule it
  encoded still binds in its general form: `is_pointer(T)` is true for bare, `unique` AND
  interface types, so a chained `else if const` on `is_pointer` covers every borrowed spelling.
- **`compile_error()` needs a STRING LITERAL.** A named constant silently emits garbage
  (`internal/issue/compile-error-non-literal-emits-garbage.md`), which is why the same poison
  message is repeated verbatim at several sites instead of being factored out.
- **Anything poisoned with `compile_error` MUST leave the caller a usable alternative, and the
  message must name it.** This rule exists because a poison message once told callers to write
  `move` at a call site that had no `move` overload.
- **VERIFICATION BAR: dtor-count AND HeapAudit oracles on LINKED binaries** (`-o out`, then RUN
  it) - never `--check`, never compile-only. Prove the oracle is NON-VACUOUS by introducing a
  deliberate leak and watching it report non-zero.
- **NEVER use call counts alone to decide WHICH overload ran.** A counting oracle cannot
  distinguish "both correct" from "both inverted" - it hid a fully inverted move-overload pair
  for hours. Assert identity or an observable effect.
- **THE BUILD DEPLOYS `cflat/core/*.cb` TO `x64/Release/core/` AND THE COMPILER READS THE
  DEPLOYED COPY.** Rebuild after editing any core `.cb` or you are testing stale code. The
  deploy COPIES but never PRUNES: a deleted core file leaves a stale artifact behind, and a
  test importing it will pass against a dead library (this happened to `list2.cb`).
- **A bare `T* p = new T();` NAMED LOCAL is itself an owning local**, auto-freed at its own scope
  exit unless explicitly moved out or obtained via an alias-returning call. This has now misled
  two separate investigations into misdiagnosing a double-free. Retrieve borrows via `.get()`.
- **`--init` serializer rule:** any new `TypeAndValue` / `StructData` / `AnnotationValue` field
  that an analysis reads MUST join the round-trip in `LLVMBackend.cpp` in the SAME change, or it
  is silently dropped on a warm cache and `expect_error` tests stop firing.
- **`alias` is pointer/interface only, and it is a PARAMETER/RETURN qualifier ONLY.** In a
  generic ARGUMENT it is rejected outright since 2026-07-20 (`list<alias T*>` -> write
  `list<T*>`). On a parameter or return it is load-bearing and unchanged: `list.get()`,
  `queue.peek()`, `hashset.add(alias T)` and the borrowed arm of `dequeue()`/`pop()` all use it.

### Deferred by maintainer ruling - do not action as part of the above

- **Lifetime / dangling for borrowed containers: ignored, none planned.** A `list<T*>` outliving
  its owner is undetectable. The hazard pre-existed unnamed; borrow-by-default makes it the
  blessed pattern, which raises the stakes but does not change the risk.
- **Blocker 1 remains unresolved, but is DORMANT.** A plain by-value param still BORROWS an
  owning value while local init MOVES it. Nothing currently exercises the gap, but the asymmetry
  is still in the language and will resurface for the next generic that stores a value param.
- **Standing question (positional asymmetry):** bare `Circle* c = new Circle();` is owning while
  bare `list<Circle*>` is borrowed - same spelling, opposite defaults by position. Endpoint (a),
  containers erase provenance so they force you to say it, is where the design landed. Endpoint
  (b), `unique` becomes the owning marker everywhere and bare `T*` uniformly means borrowed,
  stays on record as the possible destination; the container migration was its dress rehearsal.
- **Known gap:** a `compile_error`-poisoned method reached ONLY via virtual interface dispatch is
  not caught. No such call exists in-repo.
- **Follow-ups parked in Part II "Open questions":** borrowed views of owning lists (D6), a clone
  story for unique containers, `--sanitize` runtime data-pointer compare for fat-pointer alias
  stores, and the enumerate-as-checklist sweep of conversion sites for unique interface values.

### Polish debt

RE-VERIFIED 2026-07-20 (see Completed ledger for the fix commit-in-progress). Five of the
seven filed items were already stale (fixed in passing by later work) and reproduced clean;
two were real and fixed:
- `delete` on a unique LOCAL: STALE. Already reports "cannot delete unique local 'n' - a unique
  local is freed automatically at scope exit ...", not "unique field".
- Dead `TypeOwnsUniquePointer` bail in `GetOrCreateMemberwiseCopy`: STALE AS DESCRIBED - no such
  bail exists inside that function (never has, per `git log -S`). The only `TypeOwnsUniquePointer`
  gate is the choke point in `CreateOverloadedFunctionCall` (before the call to
  `GetOrCreateMemberwiseCopy`), and it is reachable (reproduced: copying a struct with a `unique`
  field and no user `copy()` hits it). Left unchanged.
- Trap A `unique field ''` for bare self-field access: STALE. Reproduced `delete n;` inside the
  owning struct's own method - reports "cannot delete unique field 'n' - ..." with the field name
  filled in, not empty.
- `unique int x[4]` pointer-flavored message: STALE. `int` is genuinely not a pointer type, so
  "requires a single-indirection pointer type such as 'unique Node* n'" is the correct rejection
  reason (confirmed against the ACCEPTED cases: `unique Node* kids[8]` and `unique IS one/slots[4]`
  both compile; only a non-pointer element type or double indirection still reports this message).
- Stale "frees and nulls" comment: STALE - that exact phrase does not appear anywhere in the tree
  (`grep -rn "frees and nulls"` matches only this plan doc's own polish-debt line).
- One duplicate test in `test_move.cb`: REAL, FIXED. Commit `54d6803` added
  `testPtrMoveOverloadIdentity()` (covering `probe()`'s plain/move overload pair with STRONGER
  assertions - it also checks the borrowed pointer stays readable) in the same commit that touched
  the inline `main()` "Test 8" block, leaving `main()`'s `probe(a)`/`probe(move a)` checks
  (`probe_plain_call_picks_borrow`, `probe_plain_call_freed_at_scope_exit`,
  `probe_picks_move_for_explicit_move`, `probe_move_freed_once`) a strict subset of the new
  function's coverage. Removed the redundant inline block; kept the `delegateProbe` sub-case
  (not covered elsewhere).
- `std::set` vs `std::unordered_set` inconsistency: REAL, FIXED. The move-dataflow tracking code
  (`NamedVariable::MovedFields`, `MovedStateSnapshot::movedFields`, all of `MoveDataflow.h`'s
  `MovedSet`/`visited`/`seen`) used `std::set` exclusively while the rest of the file's dedup/seen
  sets (`TypeOwnsUniquePointer`, `fullDestructorInProgress_`, etc.) use `std::unordered_set`. None
  of the `std::set` usages relied on sorted iteration (only `insert`/`count`/`erase`/`operator==`),
  so switched all of them to `std::unordered_set` to match the dominant convention.

### Open issues owned by this workstream

Filed and NOT fixed. Container work is done. The first nine are the compiler-side remainder;
the last two are the RAII migration's two documented SKIPS (see NEXT item 4). The three core
teardown gaps the field-migration survey filed (`rwlock-os-state-never-destroyed`,
`threadpool-continuation-ctx-leak-on-drop`, `unguarded-double-init-leaks`) were all FIXED on
2026-07-20 and their files deleted, as was `duplicate-add-leaks-unique-value.md`.

**The two-defects-or-one question is SETTLED: one defect.** Verified 2026-07-20 by a 2x2
matrix on linked binaries - the controlling variable is `return move r`, not the `move` return
TYPE and not a container. Both original issue files stated repros that were wrong on
load-bearing points and were consolidated into
`move-return-named-struct-local-strips-owned-bits.md`. The plan's own conjecture (ownership
flags "not surviving") was also wrong: the compiler ACTIVELY emits an `and ..., 0x7FFFFFFF` to
strip the owned bits. Note this is the one time the one-bug hypothesis HELD - the `unique
IFace` cluster went the other way, so keep testing it rather than assuming either outcome.

| File | What |
|---|---|
| `unique-iface-scalar-field-not-released.md` | scalar `unique IFace` field (written or substituted) is released by nothing - the fat-pointer half of NEXT item 3 |
| `move-return-named-struct-local-strips-owned-bits.md` | `return move <named struct local>` is misclassified as a borrow return and STRIPS the owned bits - silent leak on any hand-written `copy()` that move-returns. Consolidates two earlier files whose repros were both wrong |
| `interface-conformance-matches-first-overload.md` | conformance matches first declared overload |
| `compile-error-non-literal-emits-garbage.md` | `compile_error(CONST)` emits a stray character |
| `if-const-no-constant-folding-path.md` | `&&` / `||` do not const-fold |
| `if-const-global-condition-crash.md` | `if const` on a global condition crashes |
| `delete-borrow-via-named-local.md` | `delete` on a borrow via a named local is not rejected |
| `generic-interface-explicit-type-arg-base-clause.md` | explicit base-clause form drops `*` / `unique` |
| `thread-cannot-go-raii.md` | `Thread` is value-copied in `ThreadPool.resize()` AND deliberately detached by `channel.operator>>`; needs `detach()` + copy suppression before RAII |
| `pools-no-destructor-shutdown-ordering.md` | `arena_channel`/`block_pool`/`page_pool` teardown has a quiescence precondition a destructor cannot check |

### Completed ledger

| Done | What | Where |
|---|---|---|
| 2026-07-16 | Part I stages 1-5: `unique` FIELD qualifier, assignment discipline, Trap B, code-review remediation, interface field contract | git history |
| 2026-07-18/19 | Part II stages 1-6: interface value ownership, `unique` in generic-argument position, unique locals, synthesized move params (D4), `dictionary`/`hashset` unique support | Part II below |
| 2026-07-19 | `alias` as a generic type argument LANDED (compiler only). `IsAliasTypeArg` deliberately separate from `IsAlias`; serialized as `o["alt"]` | - |
| 2026-07-19 | Prerequisites: both move-overload defects FIXED, `is_copyable(T)` landed, `is_pointer(IShape)` flipped TRUE | - |
| 2026-07-19/20 | **`list` migrated to borrow-by-default**, `IList<T>` reshaped with no `move`, `take()` removed, six code-review findings fixed | commit `6528268` |
| 2026-07-20 | **`hashset` + `dictionary` migrated.** Fixed a compiler bug: the move/borrow tie-break ran only on the perfect-match tier, so a LITERAL key made `d.add(1, x)` silently consume `x` | commit `7f5d433` |
| 2026-07-20 | **`hpc/btree` migrated** - the last container on its own convention. Fixed a real duplicate-key bug that freed the caller's value. `_freeValue`/`_freeKey`'s move-param workaround SURVIVED and had to (its body is now EMPTY - the param's scope-exit destructor IS the release) | commit `54d6803` |
| 2026-07-20 | **`array` migrated** | commit `54d6803` |
| 2026-07-20 | **`queue` + `stack` migrated** - `_placeAt`/`_releaseAt`, plain + guarded-`move` insert, `dequeue()`/`pop()` return kind selected by member-scope `if const` (`alias` for a borrowed element), release-walk destructor. Fixed a pre-existing `queue._grow()` capacity bug: `if (_size >= _capacity)` ignored `_front`, so enqueueing after a partial drain wrote PAST the buffer | commit `54d6803` |
| 2026-07-20 | **`unique IFace` cluster: 2 of 3 FIXED** (+ a third, unrelated bug found with them). Owning fixed-array locals were never walked at scope exit (`unique C*[4]` leaked identically - never interface-specific); substitution set `IsUniqueTypeArg` on a `V* out` param so a POINTER TO the owning location read as a sink and dangled; `IsOwningInterfaceValue` tested only `IsUnique`, which substitution never sets, so `btree<K, unique IFace>` freed nothing. Unblocked `btree<K, unique IFace>` | commit `4cce536` |
| 2026-07-20 | **`alias` REMOVED as a generic type ARGUMENT** (NEXT item 2). Bare means borrow, so `list<alias T*>` was a synonym for `list<T*>`. Chose a HARD ERROR over silent acceptance so the dead spelling cannot rot. Deleted: `StripAliasQualifier`, `kAliasQualifierPrefix`, `MangleTypeArg`'s `alias_` token, `PeelTypeArgSuffix`'s alias out-param, the `is_alias(T)` intrinsic, `TypeAndValue.IsAliasTypeArg` and its `o["alt"]` cache entry. `IsAlias` (param/return) UNTOUCHED. `dictionary`/`hashset` poison messages now recommend the bare spelling | commit `4cce536` |
| 2026-07-20 | **Synthesized destructor now RELEASES a `unique` fixed-array FIELD** (NEXT item 3). `GetOrCreateFullDestructor` routes `unique T* f[N]` / `unique IFace f[N]` - written, or reached through a `unique` generic type argument (`IsUnique \|\| IsUniqueTypeArg`) - through the existing `EmitOwningUniqueArrayCleanup` walk, with the member builder and `currentFunction` temporarily retargeted at the wrapper body. `btree` took option (a): `_clearValue` now abandon-clears a `unique` slot exactly as it already did for an owning VALUE slot, so every stale split/borrow duplicate reads null and the node destructor frees each live slot once. `_freeValue`/`_freeKey` are UNCHANGED and still needed (remove/overwrite/clear happen before teardown). Declaration-time rule relaxed: `unique T* f[N]` is now accepted and the "fixed arrays are not supported yet" message is DELETED. Interface fields stay rejected in EVERY shape - scalar `unique IFace x` is refused by the same single-indirection rule, so relaxing only the array form would be incoherent; that message never blamed the removed limitation | commit `4cce536` |
| 2026-07-20 | **Core manual-lifecycle types went RAII** (NEXT item 4). `mutex`, `rwlock`, `condvar`, `event`, `semaphore`, `barrier` gained destructors after a copy-path audit found ZERO copy paths for each (including transitively, through `latch`, `stream`, `barrier`, `ThreadPool`, `NumaDomain`, and the `BucketAllocator` -> `block_pool` -> `arena_channel` chain). Idempotency was already free - every `os.*_destroy` nulls its slot - so `numa.cb:261`, `barrier.destroy()` and `~stream()` did not become double releases. `Thread` SKIPPED (real copy path + deliberate detach); pools SKIPPED (quiescence precondition). Oracle: peak RSS over 800k lock cycles, 1.5 MB vs 43.8 MB for a negative control - HeapAudit is VACUOUS here (lock state is `calloc`'d, not `new`ed) | commit `4cce536` |
| 2026-07-20 | **`stream.init()` and `Thread.start()` re-entry guards** - both now reject a second call with a diagnostic + `abort()` (the `list._checkBounds` idiom) instead of leaking prior state; `start()` also no longer abandons a running thread that could never be joined | commit `4cce536` |
| 2026-07-20 | **`return move <named struct local>` no longer strips the owned bits.** `ParseMoveExpression`'s struct-VALUE branch never set `lastOwningResult`, so the return site classified it as a BORROW and emitted `and i32 %x, 0x7FFFFFFF`; the caller got a non-owning struct, its destructor found nothing, and every owning field (e.g. a `string`) leaked. Hit any hand-written `copy()` that move-returns, with OR without a `move` return type, container or not. Setting `lastOwningResult = true` in that branch is the whole fix - it also drops the stray `dtorfull` on the zeroed source. Two suggested extras were REJECTED after measurement: carrying `CallerName` re-marks the origin in the move tracker (false 'use of moved variable'), and widening `ComputeReturnsOwned` to accept a `move` struct-VALUE return type DOUBLE-FREES (`move list<T> copy()` results become owned temps while the receiving local also destructs) | working tree |
| 2026-07-20 | **`threadpool` continuation-drop leak FIXED, plus a hang found with it.** The queue-saturation path now releases `cont.ctx` through `__threadpool_drop_ctx` outside the pool lock. Required a new `TaskHandle._contDtor` field: `then()` was silently DISCARDING the caller's `ctxDtor` when the continuation was deferred, so the drop path could not have used the right release. Companion bug fixed on the same branch: the drop never published `done` on the continuation's handle, so any caller holding it spun forever in `~TaskHandle`. Both pinned by `testContinuationDroppedOnSaturation` (`Test/test_threadpool.cb`), verified non-vacuous | commit `4cce536` |
| 2026-07-20 | **`dictionary<K, unique V*>.add()` duplicate-key leak FIXED.** Under borrow-by-default `add(alias K key, V value)` takes a plain parameter that IS a synthesized move sink once `V` is `unique`, so the bare `return false;` on the duplicate path dropped an already-transferred value. Added `_freeValue(move V value)` with an EMPTY body (the move param's own scope-exit destructor is the release - `btree.cb:244`'s pattern) called under `if const (is_unique(V))`. Measured `dtor=1 leaks=1` before, `dtor=2 leaks=0` after. `hashset` audited and NOT affected: `_slot()` poisons any unique-element instantiation with `compile_error`, so `hashset<unique T*>` never compiles. Issue file deleted | UNCOMMITTED (staged) |
| 2026-07-20 | **Synthesized destructor's SCALAR arm widened to `IsUnique \|\| IsUniqueTypeArg`** (NEXT item 3, second pass). A scalar field made `unique` purely by substitution was released by nothing: `Holder<unique C*>` measured `dtor=0 leaks=1`, now `dtor=1 leaks=0`. All three suites unchanged from baseline (448/0/8, 35/0, 152/0) because core has NO such field - the benefit is application-level generics, so the suite could not pin it and `Test/test_collection_leaks.cb` gained `LeakGenSlot<T>` legs in both the `unique` and the borrow direction. Scalar `unique IFace` remains unreleased (fat pointer fails the `f.Pointer` guard) and is filed separately | UNCOMMITTED |
| 2026-07-20 | **Interface array FIELD sizing bug FIXED** - `IFace v[N]` allocated ONE element with a correct 16-byte stride, silently clobbering everything after it. Plus a second bug found while verifying: the subscript handler never refreshed `interfaceVar`, so `a[i].method()` dispatched off the stale base address (affects LOCALS too) | commit `4cce536` |

---

# Part I - the `unique` field qualifier

Created 2026-07-16. **Stages 1-5 DONE (2026-07-16).** Both open decisions settled
(2026-07-16): the keyword is `unique`, and recursive chains are rejected. See Decisions.

Complements [ownership-move-alias-discipline](ownership-move-alias-discipline.md), which
stays correct for *parameters*; this part addresses *fields*, which that note does not cover
(and see the Appendix - that note has three factual errors worth fixing).

Error when a `unique` field's pointee type transitively reaches a `unique` field of the same
type. Transitive, not a `field type == my type` name test: `A` owning a `unique B*` while
`B` owns a `unique A*` is the same cycle one hop longer.

**Why: there is no good answer in the general case.** Both mechanisms the compiler could
synthesize are defective, in ways that depend on data the compiler cannot see:

- **Recursive teardown** (the natural synthesis) costs one stack frame per link on a chain,
  and one per level on a tree. A balanced tree is ~5-20 frames and fine; a sorted-insert BST
  or a long `next` chain is N frames and overflows during teardown, far from the code that
  built it. C++ has exactly this bug with `unique_ptr` and has never fixed it.
- **Iterative teardown** (worklist instead of self-call) is correct at any depth, but needs a
  heap worklist allocated *during destruction*, which can itself fail under memory pressure.

The shape that is safe is a property of the data, not the type - and the author is the only
one who knows it. So the author writes the destructor.

The chain-vs-tree distinction is what makes this non-obvious and is worth recording: the
original argument for rejecting used only `unique JsonNode* next` (a chain, N frames), which
made rejection look free. It is not free - it also rejects trees (`btree_node.children[17]`,
`btree.cb:77`, whose pointee is `btree_node<K,V>` itself), which recurse only per level.
Rejection was briefly reversed on that basis and then re-confirmed: allowing trees means
allowing degenerate trees, and there is no sound static test separating them. Heuristics
considered and rejected: "reject 1 self-pointer, allow 2+" (a skewed binary tree is linear
too; a 3-element chain is harmless).

**Consequences, accepted:**

- `btree_node` keeps its hand-written destructor.
- `json.cb:208` / `xml.cb:211` keep theirs - they opt out over the arena/heap duality anyway.
- cflat still accepts a hand-written `~JsonNode() { delete next; }`, which recurses
  identically. We decline to *synthesize* a footgun the author may still write deliberately,
  knowing their data. The line is that synthesized code should never need auditing.

## What Part I does not do

- ~~Field-only~~ **SUPERSEDED by Part II:** `unique` is now legal on locals, params
  (synthesized move), and generic type arguments (`list<unique Node*>`). Return position
  remains unsupported (transfer out is `move`, borrow out is `alias`).
- **No lifetimes.** Like C++, a borrow can still outlive its owner. Trap A is caught only in
  the callee, at a field store from a borrowed parameter. Rust's guarantee needs the escape
  analysis the discipline note rules out permanently.
- **Does not touch the value-type path.** `string` keeps its runtime owned bit (`_len` high
  bit, `LLVMBackend.h:2613`, `:2620-2627`). `unique` is a static, compile-time property. The
  two mechanisms stay separate and cover different things.

# Part II - interface value ownership and `unique` in generic-argument position

Promoted 2026-07-18 from `internal/issue/list-interface-element-copy-imperfect.md` (deleted).
Promoted because the container symptom is not fixable in the container - it needs a
language-level answer to what an interface value owns.

### Settled decisions (ballot, ruled 2026-07-18)

- **D1 - positions.** `unique` legal in any declaration position (locals, fields, type
  arguments), only on pointer or interface types. `unique int` / `unique Circle` (value
  struct) = compile error "unique requires a pointer or interface type".
- **D2 - whole-type qualifier (user ruling).** `unique` applies to the ENTIRE declared
  pointer type - `unique Circle**` is one owning value that is, at the end, still a pointer.
  No per-star binding. `is_unique` = leading `"unique "`, `is_pointer` = trailing `*`; both
  hold on `unique Circle**`.
- **D3 - signatures (amended after stage 3).** A `unique`-typed PARAM is legal and is exactly
  a synthesized move param (D4) - the two spellings are defined equivalent, so they cannot
  drift. `unique` in RETURN position stays unsupported; transfer out is `move`, borrowing out
  is `alias`. (Original D3 forbade params too; D4 superseded that, and the two err_ tests
  asserting the old field-only restriction were deleted in stage 3.)
- **D4 - synthesized move params (user ruling).** Callee declares the ownership sink; no
  call-site `move`; `shapes.add(c);` transfers and nulls `c`. A unique value passed to a
  plain-pointer parameter is a borrow; ownership stays with the caller. (Supersedes two
  earlier ideas: blanket implicit move-on-pass AND required call-site `move`.)
- **D5 - borrows and moves through alias.** Bare local from a borrow is borrowed (no
  IsOwning). `unique X* u = l[i];` without move = error. `move l[i]` is allowed: nulls the
  slot in place, size unchanged (the std::move(v[i]) idiom); `take()` is
  remove-and-transfer.
- **D6 - no conversion** between `list<unique T*>` and `list<T*>`; distinct instantiations,
  overloads do not cross. Borrowed views of owning lists = future work.
- **D7 - `is_unique` outside a generic context** returns 0, same convention as `is_pointer`.
- **D8 - teardown + F3 dependency.** Single dtor branch with null skip; the interface leg
  needed F3 (interface null-compare). Resolved: F3 was already on master.
- **D9 - `delete` on a borrowed raw pointer** stays legal (C compat); double-free is
  `--sanitize` territory, no static diagnostic.
- **D10 - canonical text.** One normalization point yields `"unique Circle*"` (single space,
  attached star); substitution map and mangling derive from it.
- **D11 - serializer.** Any new `TypeAndValue`/`StructData` bit joins the `--init` round-trip
  in the same change (standing repo rule).
- **D12 - insertion params are plain `T`; ordinary passing semantics decide (user ruling
  2026-07-19; STRING LEG AMENDED same day - see below).** No container-specific rule:
  unique T => synthesized move (D4); owning value struct => auto-move (existing language
  default); primitive => copy; bare pointer/interface => borrow. Supersedes the explicit
  `move T` insertion params from Stage 5/6 and resolves the borrowed-list `add` leak trap.
  **AMENDMENT (premise error, corrected 2026-07-19):** the ruling as first recorded said
  `string` => COPY, "consistent with `string b = a`". That premise is FALSE - `string b = a`
  MOVES (verified: use-after-move on the next read; `core/string.cb:12` documents
  "move-by-default"). The consistent semantics is therefore TRANSFER: `l.add(s)` moves and
  `l.add(s.copy())` copies, exactly as `string b = a` / `string b = a.copy()` do. With that
  correction the string leg needs no call-site `move` and no copy-by-default, which is what
  makes unblock option (c) viable. Behavior change is then confined to bare
  pointers/interfaces (consume -> borrow). See live section item 2 and Stage 7.

### copy()

Bitwise copy of unique elements is a double-free. Unique instantiations are MOVE-ONLY at
first: `copy()` on `list<unique T*>` / `list<unique IShape>` is a compile error, via the
`compile_error` builtin (deferred-poison mechanism; see git history). A later clone story
for interfaces needs a vtable copy slot every implementor fills; for pointers, an element
`.copy()` through `new`. Not in scope.

## Interaction with interface fields (F1)

`internal/plan/interface-fields-feasibility.md` F1 makes interfaces more value-like. Layering
rule that keeps the two plans independent:

- **Fields are owned by the implementor object, or by a `unique` interface value that owns
  the object - never by a borrowed view.** All field-store rules apply through the interface
  path via the `IsInterfaceField` flag (flag-based, never GEP-shape-based - the spike's
  hardest-won lesson), on BOTH store paths (assignment and `EmitOneFieldInit` brace-init).
- **Teardown is free.** The vtable fullDtor is the implementor's dtor, which already destroys
  its owned fields. Whichever container knows to invoke the dtor slot gets fields for free.
- Audit before F1 lands: the shared field-store helpers must classify "is a field store" by
  flag, not GEP shape, or interface fields leak on the brace-init path only.

## Open questions

- **Positional asymmetry** - see live section item 6.
- **Move out of an interface field** (`string s = move iface.title;`): mutation-at-a-distance
  through a view. Language philosophy is consistent with allowing it; forbidding first is the
  reversible choice. Needs a call when F1 lands.
- ~~Does the same hole exist in `dictionary<K,V>` / `hashset<T>`?~~ Assumed yes (shared
  mechanism); being closed by Stage 6, in progress.
- Alias-reject cannot see through fat pointers (two views of the same object are statically
  indistinguishable). Accepted gap - same as raw pointers today; a `--sanitize` runtime
  data-pointer compare before owned-field stores is a cheap follow-up.
- Return-position and other conversion sites for unique interface values (ternary arms,
  brace-init of interface fields, `return move`, default args) - enumerate as a checklist,
  do not fix as-found (the brace-init parity lesson).

## Related

- `internal/plan/ownership-move-alias-discipline.md` - parameter discipline
- `internal/plan/interface-fields-feasibility.md` - F1; see "Interaction with interface
  fields"
- `internal/plan/ownership-sanitizer.md`, `internal/plan/move-dataflow.md` - runtime and
  dataflow companions to the static rules here
- `internal/issue/brace-init-field-store-not-at-parity.md` - the durable store-path-parity
  rule (shared helpers from the start), learned partly from this feature
- `Test/test_interface.cb:95-106` - both boxing forms
- `Test/test_core.cb:639-661` - the owning-list API contract
- `Test/test_collection_leaks.cb` - leak regression oracles (HeapAudit + dtor counts)
