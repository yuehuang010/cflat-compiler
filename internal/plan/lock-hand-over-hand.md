# Lock hand-over-hand (staggered lock/unlock) - DESIGN IN PROGRESS

Status: NOT FINISHED. This captures the design exploration so far. No code written yet.
No syntax, staging, or acceptance criteria finalized. Open decisions listed at the end.

## Goal

Support staggered locking for concurrent B-tree / skip-list traversal (a.k.a. lock
coupling / hand-over-hand): lock child before releasing parent, walk down, releasing
each node's lock only once the next is held. This inherently crosses lexical scopes,
so the existing scoped `lock(m) { ... }` form cannot express it.

Original ask: an explicit non-scoped form, e.g. `lock(mtx1); ... unlock(mtx1);`.

Constraint from maintainer: raw `mtx.acquire()`/`mtx.release()` must stay legal
(see internal/plan/lock-capability-interface.md phase 5; the concurrent-btree work
depends on it). This feature is additive on top of raw acquire/release.

Key maintainer constraint (decisive): the guard/capability is COMPILE-TIME ONLY.
It must NOT wrap or project the data. `node.balance` must lower to the exact same
load/store it does today. The guard exists only in the checker to permit access.
This rules out the Rust `Mutex<T>` "access the data through the guard" model.

## Current lock feature (baseline)

- Scoped statement `lock(expr) { body }` (CFlat.g4 lockStatement/lockClause,
  ~line 560). Handled inline in MainListener.h ParseStatement ~5427-5606.
  Not ANTLR enter/exit - the front end is a hand-written recursive walker.
- Capability-driven: `[Capability(ILockable)]` etc. (core/interfaces.cb) provides
  acquire/release (and acquire_read/release_read). mutex (core/mutex.cb) and rwlock
  (core/rwlock.cb) opt in. No lock TYPE is hardcoded (kCapabilities table,
  MainListener.h ~141-160).
- Release on scope exit is emitted by the scope-teardown machinery, NOT an RAII
  object: the frame stores lockCleanup (LLVMBackend.h:834), released in
  EmitDestructorsForScope (LLVMBackend.h:1864). Fires on every exit path.
  NOTE: today lockCleanup records only acquired[0] - single-lock cleanup slot.

### GuardedBy is TEXTUAL (the crux)

- There is no `[GuardedBy(...)]` annotation. Guarding is a lock field-group inside a
  struct: `lock(mtx) { int balance; node* child[16]; }` (CFlat.g4 lockFieldGroup
  ~810). Fields in the group get TypeAndValue.GuardedBy = "mtx" (bare guardian text).
- currentLockSet is `unordered_map<string, LockMode>` (MainListener.h:2036), keyed on
  CANONICAL LOCK TEXT: arrow-normalized (`p->mtx` -> `p.mtx`), mode-suffix-stripped
  (`c.rw.read` -> `c.rw`). Helpers: NormalizeLockText / ArrowNormalizeLockText /
  LockTextMode (MainListener.h ~636-702).
- A guarded write is checked by RECONSTRUCTING a string: receiverName + "." + guardian
  (e.g. "a" + "." + "mtxA" = "a.mtxA") and doing currentLockSet.find(...)
  (MainListener.h:13237 member access, CheckGuardedWrite:18587, self-field:17400,
  global:18562, call-site required-locks CheckCallSiteLocks:18614). Mode value gates
  read vs write (write under Shared/Optimistic rejected). No resolved-symbol or
  type+field matching anywhere - purely source text.
- Pushed/popped around a lock(){} body (MainListener.h:5574) via save/restore.

Why textual GuardedBy defeats naive staggered lock/unlock: the required-lock key is
anchored to the RECEIVER'S SOURCE NAME. Hand-over-hand renames the same node as the
pointer walks (`cur = next`), so the lock's text at acquire time ("next.mtx") never
matches the access text after reassignment ("cur.mtx"). A free-floating
`lock(x);`/`unlock(x);` plus textual lock-set cannot verify the coupled path, and
strict per-scope balance PROVABLY rejects the canonical traversal (lock acquired in
loop body as "next.mtx" is never unlocked in that body; final unlock underflows).

## Design directions considered

- C++ std::unique_lock: movable owning handle; release tied to object lifetime, moved
  forward for coupling. Ownership tracked by the OBJECT, not lexical scope or name.
- Rust MutexGuard: release on Drop; access data THROUGH the guard (guard derefs to
  data), so the borrow checker follows moves and access is gated by possession.
  REJECTED here: maintainer requires compile-time-only, no data projection.

Shared principle: the unit of lock ownership is a value/state transferred by move,
never a lexical scope and never a textual receiver name.

## Chosen direction (tentative): capability rides on the POINTER VARIABLE

Model the held lock as a COMPILE-TIME flow-sensitive typestate on the pointer variable
itself (a sibling to IsMoved), not a separate runtime guard object and not data
projection:

    lock(cur.mtx);                 // typestate: cur holds mtx-group
    while (!cur.isLeaf) {
        node* next = cur.child[i];
        lock(next.mtx);            // next holds mtx
        unlock(cur.mtx);           // cur clears; emit release(cur.mtx)
        cur = next;                // ASSIGNMENT TRANSFERS held-lock state cur<-next
    }
    use(cur.balance);              // cur holds mtx -> verified
    // scope exit: cur still holds -> auto release (leak safety)

`cur = next` is exactly the assignment the existing ownership machinery already handles.
If the held-lock capability transfers on assignment the same way IsMoved does, it moves
with the pointer for free - no alias graph needed (CFlat has none), no renamed-receiver
problem, no runtime object, no data impact. Access at cur.balance sees the capability
created at next.mtx because it rode across the assignment.

### Non-negotiable rules for the typestate (from Fable design review)

Frame this honestly for what it is: a must-hold, name-keyed, FUNCTION-LOCAL lint - the
same bar the textual checker already sets. Do not creep toward soundness against
adversarial code. Given that bar, three rules are load-bearing:

1. CLEARING matters more than transferring. `cur = next` transfers, fine - but any
   assignment whose RHS is NOT a tracked variable carrying the capability
   (`cur = cur.child[i]`, `cur = f(...)`), and any address-taken escape (`&cur` passed
   to a callee), must CLEAR cur's held-state. Silently keeping the typestate across an
   untracked RHS is the one genuinely unsound outcome: the checker then believes the
   lock guards a node the pointer no longer points to. This is the single most
   important correctness rule.
2. The capability is LINEAR, not copied. On `p = cur`, cur LOSES it (exactly like
   IsMoved). Copying to both names manufactures TWO scope-exit auto-releases of one
   acquisition - a double-release the checker itself creates. Linear transfer
   conservatively rejects lock-via-a / unlock-via-b aliasing; acceptable for a lint.
3. Key the state on GUARDIAN IDENTITY, not text: store `(guardian field-id in the
   struct, LockMode)` on the NamedVariable, not a reconstructed string. A struct can
   have multiple lock groups, and this removes the arrow/mode-suffix normalization
   hacks for tracked variables.

`lock` must require the capability ABSENT (catches re-lock); `unlock` must require it
PRESENT (catches double-release). Calls while holding are opaque - the capability
survives (matches the existing function-local bar). Auto-release must be emitted on
EVERY exit edge (break/return/continue), same as the existing scoped-lock unwind.

### What already exists to reuse (verified)

CFlat HAS genuine flow-sensitive per-variable state - use-after-move:
- NamedVariable.IsMoved / MovedFields (LLVMBackend.h ~694), field-granular, evolves
  statement-by-statement. Mutators MarkVariableMoved/Unmoved/FieldMoved/FieldUnmoved
  (LLVMBackend.h:14134-14201). Use check MovedUseSubject (14208), called at every load.
- Transfer on assignment: on `a = b`, MainListener.h:8227 invalidates b
  (MarkVariableMoved) and revives a (MarkVariableUnmoved). This is the hook a lock
  capability would piggyback on.
- if/else merge: SaveMovedState / RestoreMovedState / MergeMovedStates
  (LLVMBackend.h:14234-14293), conservative union, snapshots all scopes. Call site
  MainListener.h:5123-5143.

### The one real gap - and why it is NOT a fixpoint (Fable review)

Loops (while/for/do/for..in) and switch do NO state snapshot/merge - single linear
walk via ParseControlledBody (MainListener.h:4664-5150); today move-safety survives
there only because storage is physically nulled at runtime (a runtime crutch, not
static soundness). Hand-over-hand lives in a loop, so this needs loop-edge handling.

KEY SIMPLIFICATION: held-lock state is a MUST-analysis (merge = intersection), so no
iterate-to-fixpoint dataflow is required. Use the cheaper INVARIANT-CHECK scheme:
1. Snapshot held-state at the loop head.
2. Single linear walk of the body.
3. Require, per tracked variable, back-edge state SUPERSET-OR-EQUAL head snapshot.

This is not an approximation of the fixpoint - it IS a fixpoint certificate: if the
back-edge re-establishes the head state, a second iteration computes nothing new, so
one pass provably suffices. Bonus: it yields a precise diagnostic ("iteration ends
without re-establishing hold on cur.mtx").

Edge rules that make the cheap check SOUND (get these exactly right or it breaks):
- `continue` is a BACK-EDGE - it must satisfy the superset check too, not just the
  bottom of the body.
- Loop-EXIT state is the MEET (intersection) of ALL exit edges - the condition-false
  edge AND every `break`, each carrying its own state. Do NOT use the head snapshot as
  the exit state (wrong in both directions).
- do/while: snapshot at BODY ENTRY; the condition runs inside the first iteration.
- Nested loops compose by induction.

This is still the main new analysis work, and it is currently absent for ALL
flow-sensitive state, not just locks - see staging note 4 on scope.

## Rough staging (NOT finalized)

1. HeldLocks state on NamedVariable (sibling to IsMoved), keyed on `(guardian field-id,
   LockMode)` NOT text + Mark/check helpers, reusing the Save/Restore/Merge plumbing.
   `lock` requires-absent, `unlock` requires-present.
2. Surface syntax `lock(x.mtx);` / `unlock(x.mtx);` (soft keyword `unlock`) driving it;
   scoped lock(){} stays.
3. Transfer-on-assignment (piggyback MainListener.h:8227), LINEAR (source loses it) +
   CLEAR-on-untracked-RHS/address-escape + scope-exit auto-release on EVERY exit edge
   (extend lockCleanup from single slot to a set).
4. Loop/switch INVARIANT-CHECK extension (MainListener.h:4664-5150), not a fixpoint
   (see gap section). SCOPE DECISION (Fable): build the snapshot/merge as ONE GENERIC
   per-variable-flow-state bundle, but POPULATE AND ENFORCE it for LOCKS ONLY in this
   change. Do NOT flip move-state onto loop-merge yet: that is a behavioral TIGHTENING
   (code passing today via the runtime-null crutch would start erroring), entangling
   test churn with an unrelated feature. Adopt move-state into the same mechanism as a
   deliberate follow-up. File the latent move-in-loop hole in internal/issue/ now.

## Open decisions

- [LEANING RESOLVED - Fable] Surface: explicit `lock(x);`/`unlock(x);` statements +
  variable-typestate, NOT a named `guard` binding. A first-class `held h = lock(cur.mtx)`
  binding just relocates the aliasing problem (tying `h` back to `cur`) and adds syntax;
  the textual-rekey hack (rewrite "next."->"cur." in the map) hardens text-as-identity
  and saves nothing. Statements-on-typestate is the re-key idea done on the right
  substrate. Confirm with maintainer.
- Scope-exit policy for a still-held explicit lock: auto-release (leak-safe, proposed)
  vs strict balance error. Strict PROVABLY rejects the coupled traversal - do not use
  for the coupled path. (Earlier the maintainer leaned strict before the text-tracking
  limitation was understood; revisit.)
- unlock pairing: only with the new lock statement, or also able to release a raw
  acquire / early-exit a scoped lock? (Double-release hazard with scoped cleanup.)
- Read vs write mode for the variable-typestate form (lock(x.rw.read) constness).
- Method calls through a held node: CheckCallSiteLocks (18614) is textual; how does the
  variable-typestate capability satisfy a method's RequiredLocks?
- Multi-guardian structs / whole-struct vs field-subset granularity.
- Loop merge: must-hold consistency check vs real fixpoint; how to report inconsistency.

## Related

- internal/plan/lock-capability-interface.md (capability model, why raw stays legal)
- internal/plan/lock-guard-mandatory.md (release-on-early-return, mode fix)
- internal/plan/optimistic-lock-coupling.md (IOptimisticLockable)
- memory: concurrent-btree-direction (raw acquire/release must stay legal)
