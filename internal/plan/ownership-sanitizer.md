# Ownership Sanitizer (`--sanitize=ownership`)

Status: M0 DONE (committed). M1 DONE. M2 DONE (via option B - the generalized null-deref
guard; the object-lifecycle shadow map was abandoned, see below). M0 (loop back-edge
move-state soundness) shipped as the move-dataflow pass (internal/plan/move-dataflow.md,
cflat/MoveDataflow.h): loop-carried use-after-move is a COMPILE ERROR. The runtime
sanitizer (`--sanitize=ownership`) now catches use-after-move / use-after-free through
ANY provenance (locals, fields, containers) at DEREFERENCE time, with zero false positives
and byte-identical flag-off (test.sh 403/0/8). DEFERRED: double-free and use-after-drop via
a non-null dangling alias (object identity) - would need allocator-level hooking (option A)
and are largely ASan's territory; not implemented.

## Goal

A debug-only, flag-gated instrumentation mode that catches ownership-contract
violations at runtime, the way AddressSanitizer catches memory violations. It
targets the bug class cflat's `move`/`unique` semantics make possible - above all
**use-after-move**, which ASan structurally cannot see.

## Concept

Every owned value carries a runtime shadow state:

```
LIVE     - binding owns a live object; safe to use / move / drop
MOVED    - ownership transferred out; reading or dropping is a bug
DROPPED  - already destroyed; freeing again is a bug
```

Instrumentation reads/updates that state at the points where ownership semantics
already happen in the compiler.

Two representations, chosen per check:

- Shadow slot (cheap, per-binding): a hidden `i8` beside each owning local.
  Catches use-after-move / double-free / leak on plain locals. Cannot follow a
  value that moves into a container.
- Shadow map (thorough, per-object): a side table keyed on object address, like
  ASan shadow memory. Needed for aliasing, ownership through containers/trees,
  heap objects. This is the expensive, genuinely-new part.

## Checks and hook points

Each hook is a place the compiler already does ownership work, so the check sits
next to existing logic rather than at a newly-discovered site.

| Check                | Hook point                                          | Emitted code |
|----------------------|-----------------------------------------------------|--------------|
| Use-after-move       | load of an `IsOwning` `NamedVariable` in MainListener | guard `state==LIVE` before load; else trap(useLoc, moveLoc) |
| Move out             | the `IsMove` transfer path                          | set source `state=MOVED`; record move location |
| Double-free          | destructor / scope-exit emission in LLVMBackend     | if `state==DROPPED` trap; else run dtor, set `DROPPED` |
| Leak                 | scope exit (locals) + program exit (shadow map)     | assert `state==MOVED || DROPPED`; else report leak site |
| IsOwning invariant   | anywhere `IsOwning` is consulted                    | assert runtime state agrees with static flag (also catches compiler bugs) |

Move-origin tracking is what makes diagnostics actionable: store the move's
`(file,line,col)` into shadow state so a trap can print both "moved here" and
"used here". Locations intern into the existing `stringPool`.

## Static / runtime split

Do not instrument what the static analysis already proves.

1. Run existing static ownership analysis first.
2. Emit runtime checks ONLY for the "cannot prove either way" set: ownership
   crossing a loop back-edge, conditional moves, ownership stored into a
   struct/array/container, ownership through a generic. Straight-line
   move-then-use is already a compile error - no runtime check needed.

Side benefit: the instrumented-site set IS the set of places the static checker
gave up. That list independently tells us where to strengthen static analysis.

### THIS SPLIT IS BROKEN AS WRITTEN (Fable review - decisive)

The split assumes a checker with THREE verdicts: proven-safe / proven-bug / unknown.
Today's IsMoved analysis emits only TWO (error or silence), and in LOOPS silence means
WRONG, not proven: Save/Restore/Merge exists for if/else but there is NO back-edge merge,
so a value moved on iteration N reads LIVE at the top of iteration N+1. Asking that
analysis "which sites are you unsure about" queries an UNSOUND oracle that is not unsure -
it is confidently wrong. So the "cannot prove either way" set is not computable from the
current analysis.

Fix, in order:
1. FIRST make the static analysis SOUND-BUT-CONSERVATIVE: at a loop back-edge, merge
   loop-exit move-state into the loop-header state (conservatively: any owned variable
   moved anywhere in the body is "possibly-MOVED" at the header). Bounded change - the
   Save/Restore/Merge scaffolding already exists for if/else. This is a LANGUAGE
   SOUNDNESS FIX, not a sanitizer feature. Was tracked as a known issue:
   ~~internal/issue/move-state-loop-merge.md~~ **FIXED 2026-07-18, issue file deleted** (shipped
   as the move-dataflow fixpoint, cflat/MoveDataflow.h; loop-carried use-after-move is now a
   compile error). NOTE the shared machinery with the lock
   hand-over-hand typestate (internal/plan/lock-hand-over-hand.md) - a "possibly-X at a
   join point" merge should be built ONCE and shared (move-state is a MAY/union analysis;
   held-locks is a MUST/intersection analysis - same plumbing, opposite meet).
2. THEN define the split properly: the runtime-instrumented set = sites the SOUND
   analysis calls "possibly-moved" that the language chooses to ALLOW anyway. Until that
   lands, the interim rule (instrument all loop-carried owned values unconditionally) is
   scaffolding, not the design.

## Trap mechanism

Runtime helper `__cflat_own_trap(kind, useLoc, originLoc)` prints a formatted
report and aborts - mirrors how ASan links a runtime. Lives in a small
flag-gated core `.cb` or C shim, linked only under the sanitizer flag. Distinct
from `CompilerManager.h`, which handles *compiler* crashes, not *compiled-program*
runtime traps.

## What is missing vs a normal compile today

Reuse (already exists):
- `NamedVariable.IsOwning` - which bindings own.
- `TypeAndValue.IsMove` + move-param detection in ForwardRefScanner - transfer points.
- Destructor / scope-exit emission in LLVMBackend - drop sites.
- Source locations (`-g`) and `stringPool`.

New (does not exist today):
- Runtime shadow state - ownership is purely compile-time now; no runtime byte exists.
- Use-site guards - plain field/var loads are not currently ownership-hooked.
- The "unprovable" set - compile today yields error OR silence, never a third bucket.
- Runtime move-origin tracking - move location is known at compile time, then discarded.
- Program-runtime trap + report shim - nothing links into user programs today.
- Per-object identity (shadow map) - ownership is tracked per-binding, never per-object. Hardest, entirely new.
- Post-monomorphization instrumentation ordering - checks must be emitted per instantiation (`Box__int`); resolve where in the pass order that is visible.

## How it differs from ASan

ASan is address-based and semantically blind: shadow memory marks byte validity,
instruments raw loads/stores, hooks malloc/free. It knows nothing about types,
bindings, or ownership.

- Overlap: use-after-free, double-free, leak - ASan is mature and good here.
- Ownership-only, ASan cannot see:
  - Use-after-move - after a move the object is still valid memory (same address,
    not freed); ASan's shadow says "addressable". The bug is semantic. This is the
    core differentiator.
  - Semantic double-free - "two owners both think they should drop this", caught at
    the ownership violation, earlier than an actual double-`free`.
  - Source-level diagnostics - "x moved into y at L40, used at L55" vs ASan's
    `free(0x7f..)` + stack trace.
  - Leak with intent - "unique left scope without being destroyed or moved out",
    even if memory is still reachable.
- ASan strictly better: buffer overflow / OOB (spatial, orthogonal), raw-pointer
  and `.c`-interop code, maturity.

Conclusion: complementary, not competing. Run both in debug. The shadow-map path
overlaps with what ASan already does well; the shadow-slot use-after-move path is
pure new ground ASan cannot cover.

## Milestones (RE-ORDERED per Fable review)

Original ordering (below) put a shadow SLOT for locals first. Fable's decisive finding:
that slot is triple-redundant - static analysis catches straight-line cases, the M0
static fix catches loops/conditionals, and the runtime-null crutch already traps the
rest. The differentiated value is entirely in the shadow MAP.

- M0 DONE (committed): loop back-edge move-state soundness, shipped as the move-dataflow
  fixpoint (internal/plan/move-dataflow.md, cflat/MoveDataflow.h). Loop-carried
  use-after-move is now a compile error. ~~internal/issue/move-state-loop-merge.md~~
  **FIXED 2026-07-18, issue file deleted.**
- M1 DONE (uncommitted working tree): `--sanitize=ownership` (+ `-fsanitize=ownership`),
  per-owning-pointer-local i64 origin slot (set on move, cleared on revive), deref guard
  `if (origin!=0 && ptr==null) __cflat_own_trap(useLoc, originLoc)`, pure-cflat trap shim
  core/diagnostic/own_sanitize.cb. Flag-off byte-identical (test.sh 403/0/8); flag-on traps
  explicit-move derefs with origin; null-COMPARES stay silent (deref-vs-compare is structural
  - only the 3 deref lowerings are guarded). Files: ArgParser.h, main.cpp, LLVMBackend.h/.cpp,
  MainListener.h, own_sanitize.cb. NOTE: implicit-move deref is now an M0 COMPILE error, so the
  runtime guard only ever fires on the EXPLICIT-move form the language permits.
- M2 DONE (uncommitted working tree), but NOT as the shadow map originally planned. The
  thread-safe object-identity map was built, hit an inherent address-reuse wall, and was
  removed in favour of option B: a generalized null-deref guard at the three deref sites,
  which needs no map and therefore no thread-safety story. See "M2 concrete design" below
  for what shipped and the wall write-up.
- Generics fold in wherever they arrive - hooks already fire per-instantiation (Box__int),
  so the old "post-monomorphization ordering" open question is RESOLVED; deleted below.

### Decision that gates whether M1-as-a-check even exists - NOW DECIDED

The open question was: conservative STATIC error for locals (Rust) vs a permissive RUNTIME
check? DECIDED by discovered language semantics (memory: explicit-move-nulls-source):
CFlat's explicit `move x` DELIBERATELY nulls the source and LEAVES IT READABLE AS NULL -
`a.item == nullptr` after `move a.item` is legal and asserted by positive tests
(test_move.cb:601, test_collection_leaks.cb:774). A Rust-style static error on read-of-
moved-source is therefore OFF THE TABLE - it contradicts the language (an attempt broke 6
test files and was reverted).

Consequences for M1:
- No static tightening of locals. M0 already made IMPLICIT loop-carried move a compile
  error; EXPLICIT `move` source-read stays permitted. That asymmetry is intentional.
- The sanitizer must NOT trap on a null-COMPARISON of a moved source (`p == nullptr`) - that
  is legal. It should trap only on a DEREFERENCE of a moved-from (null) source (`p->f`, `*p`,
  `p[i]`) - the operation that actually faults. This is the key M1 constraint.
- Runtime shadow (the origin slot) is justified for locals purely for DIAGNOSTICS on deref,
  not to forbid a use the language allows. The shadow MAP (M2) remains the place identity is
  genuinely dynamic (containers, trees, heap, aliasing).

### M1 concrete design

Runtime-only, control-flow-agnostic (does NOT depend on the static pass), gated on
`--sanitize=ownership` (implies `-g`).

1. CLI: add `--sanitize=ownership` (+ `-fsanitize=ownership` alias) mirroring the
   `-fsanitize=address`->`asan` plumbing (ArgParser.h ~66; SetAsan in main.cpp ~522 ->
   add SetSanitizeOwnership). A `LLVMBackend` bool gates all emission below.
2. Trap shim: `__cflat_own_trap(useLine, useCol, originLine, originCol)` in a flag-gated
   core shim (model on core/diagnostic/thread_fuzz.cb's install/link pattern). Prints
   "ownership violation: value moved at <origin>, dereferenced after move at <use>" and
   aborts. Linked only under the flag.
3. Origin slot: for each owning-pointer LOCAL (NamedVariable.IsOwning), under the flag,
   allocate a hidden i64 origin slot beside it, initialized 0 (= live). Encodes the move
   site as (line<<32|col), 0 = not moved.
4. Set on move: at the move-null-store points (ParseMoveExpression's null store; the
   move-param transfer / ApplyMoveParamTransfer), also store the encoded move location into
   the source local's origin slot.
5. Clear on revive: at the reassignment revive-LHS path (where MarkVariableUnmoved fires),
   reset the origin slot to 0.
6. Guard at DEREFERENCE (NOT at null-compare): at the central owned-pointer deref lowering
   (arrow/`*`/subscript load of an owning local), under the flag, emit
   `if (originSlot != 0 && ptr == null) __cflat_own_trap(useLoc, decode(originSlot))`.
   A `p == nullptr` comparison is not a deref and is left un-instrumented (legal).

Scope for M1 v1: owning-pointer LOCALS only (per-binding origin slot). Ownership that moves
into a container/field/tree is M2 (shadow map) - out of scope. Owning-VALUE structs (no
natural null) are also deferred to M2.

### Shadow slot vs null crutch (why M1 is a shim, not a state byte)

For pointer-shaped owned locals the nulled storage IS ALREADY a one-bit shadow state,
maintained at exactly the right transfer points; a deref of a moved value traps today.
What null does NOT give: (a) move-origin diagnostics - the trap is anonymous, not
"moved at L40, used at L55" (the real value); (b) non-deref misuse - passing/storing/
comparing the null does not trap at the bug; (c) MOVED vs DROPPED distinction. So M1 is:
at instrumented use sites emit `if (ptr == null) __cflat_own_trap(...)` with an interned
move-origin in a small side slot - same diagnostics payoff, half the mechanism, state
derived from storage you already null. A separate shadow byte is forced ONLY for owned
values with no natural null (by-value owned structs) - handle those when they arise.

### M2 concrete design (the shadow map)

Object-identity runtime shadow, thread-safe from day one. Gated on `--sanitize=ownership`
(same flag as M1; M2 extends M1's shim). Where M1 tracks per-BINDING (owning-pointer local
origin slot), M2 tracks per-OBJECT (heap address) so ownership that crosses into a
container/field/tree is followed.

Shadow structure: a global SHARDED map `addr -> {state, originLine, originCol}` with
state in {LIVE, MOVED, DROPPED}. Sharding = N buckets each guarded by its own core mutex
(shard = (addr>>4) % N) - debug-only, so a locked map beats a fragile direct-mapped shadow.
Prefer PURE-CFLAT (core dictionary + core mutex, sharded) to stay cross-platform like M1's
own_sanitize.cb; a `.c` shim is the fallback if the cflat map proves too slow. Runtime API:
`__cflat_own_register(addr,line,col)`, `__cflat_own_drop(addr,line,col)`,
`__cflat_own_use(addr,useLine,useCol)`, and (Stage 2) `__cflat_own_move(slotAddr,line,col)` /
`__cflat_own_use_slot(slotAddr,useLine,useCol)`.

M2 RESOLVED via option B (below). The object-lifecycle shadow map was removed - it hit an
inherent address-reuse wall. What shipped instead: EmitOwnDerefGuard (LLVMBackend.h) now
traps on `ptr == null` at the three deref sites (p->f 13598, *p 10969, p[i] 14664), skipping
`?.` null-conditional access (nullConditionalPending) which legitimately tolerates null. A
tracked owning local carries a move-origin slot (M1) for a "moved at X" message; other
provenances (fields, container slots) trap with a generic null-deref message. The trap shim
own_sanitize.cb is now just __cflat_own_trap (no map, no mutex). Verified: use-after-move
through a field traps; `?.` on a moved field stays clean; flag-off byte-identical. The wall
write-up is kept below for the record.

--- ORIGINAL WALL (superseded by option B) ---
M2 Stage 1 STATUS - HIT A WALL (address-reuse false positives). The shim (flat POD global
arrays + one global mutex; a struct-array-of-mutexes did NOT reliably zero-init) works for
direct alias cases: use-after-drop and double-free via a dangling alias trap correctly. BUT
on real owning-heavy tests it false-positives pervasively: an address DROPPED via one path,
then REUSED by an allocation that does not go through the tracked `new`/register (array
buffers, container-internal mallocs, or excluded shapes), stays DROPPED, so a later deref of
the legitimately-live reused object reports a phantom use-after-drop. Restricting to
single-object new/delete only made it worse (array reuse of a dropped single-object address
now stays DROPPED). This is inherent to an object map WITHOUT allocator-level coverage.
Two sound paths (DECISION NEEDED before continuing):
  (A) Allocator-level hooking: register/drop at `operator new`/`operator delete` (or the core
      allocator) so EVERY allocation registers and EVERY free drops - then reuse always
      re-registers LIVE. Sound, but bigger, and overlaps ASan (loses the ownership angle).
      Likely also needs a quarantine to reason about reuse ordering, ASan-style.
  (B) PIVOT to Fable's reshaped Stage 2 and DROP the object map as a decider: the detection
      is just a generalized null-deref guard on EVERY owned-pointer deref (the null-in-slot
      survives realloc); the address map, if kept at all, is advisory-origin-only (never
      decides a trap, so staleness only weakens a message). No register/drop lifecycle, no
      reuse false positives. This is what Fable recommended.
Flag-OFF remains byte-identical (test.sh 403/0/8); the M2 code is flag-gated so normal builds
are unaffected. Current working tree has the (false-positive-prone) object-map hooks in place.

M2 Stage 1 - object lifecycle (double-free + use-after-drop), the unambiguous foundation:
- register LIVE at `new` (owning allocation lowering in LLVMBackend).
- at `delete` / owned scope-exit drop (EmitDestructorsForScope, LLVMBackend.h:1812): call
  `__cflat_own_drop` FIRST - if already DROPPED, trap (DOUBLE-FREE with both drop origins);
  else mark DROPPED. Then free.
- at owning-pointer DEREF (reuse M1's 3 deref sites): also call `__cflat_own_use` - if
  DROPPED, trap (USE-AFTER-DROP with drop origin). This is object-identity, thread-safe,
  and works on CFlat-native heap; overlaps ASan but adds ownership origin and is the
  substrate Stage 2 builds on.

M2 Stage 2 - slot-level use-after-move through containers (the concurrent-B-tree payoff,
HARDER, review before building): moving OUT of a container slot / `unique` field
(`y = move vec[i]` / `move node.child`) leaves the OBJECT live, so object-state cannot see a
later read of the emptied SLOT. Track the SLOT ADDRESS's move-origin (mark on the move,
check on a subsequent slot read), analogous to M1 but keyed on the slot's heap address
instead of a local alloca. Open: which slot reads to instrument, and interaction with the
object-state map. Get an adversarial review before implementing.

Ordering: Stage 1 first (foundation, clearly correct, thread-safe). Stage 2 after review.

### Original ordering (superseded, kept for context)

1. Use-after-move on locals (shadow slot, no shadow map).
2. Double-free + leak on locals. NOTE: the locals LEAK check is near-vacuous -
   EmitDestructorsForScope already drops any owned local not moved out, so a local can
   only leak by escaping through a raw pointer, which is a shadow-map problem anyway.
3. Shadow-map path: ownership through containers/trees, heap object identity.
4. Generics + threads.

## Open questions / risks

- Aliasing through data structures: the shadow slot cannot follow a `unique` moved
  into a `vector<unique<T>>` or tree node - needs the shadow map, which is where
  most effort lives and where the concurrent-B-tree value is.
- Generics: RESOLVED (Fable + recon) - hooks already fire per-instantiation (Box__int);
  no separate post-monomorphization pass needed.
- Threads: NOT a deferred milestone - shadow map must be thread-safe (atomics or
  per-thread shadow) from day one; the payoff cases are concurrent (M2 above).
- Overhead: state load + compare per instrumented access is cheap for locals; gate
  the shadow-map heap path behind `--sanitize=ownership=full`.
- `--sanitize=ownership` should imply `-g`. Do NOT disable static-elision fast paths
  (Fable): a sanitizer that CHANGES which code paths execute stops being an observer.
  Instrument the paths that actually run.
- "IsOwning invariant" runtime assert (runtime state agrees with the static flag) is a
  KEEPER - cheap compiler-bug detector, retain it.

## Related

- ~~internal/issue/move-state-loop-merge.md~~ **FIXED 2026-07-18, issue file deleted.** - was
  the M0 static soundness fix (loop back-edge move-state merge) that this plan's split depends
  on; now shipped as the move-dataflow fixpoint (internal/plan/move-dataflow.md).
- internal/plan/lock-hand-over-hand.md - also pointer-variable typestate; the loop-merge
  "possibly-X at a join point" machinery should be built ONCE and shared (move-state =
  MAY/union; held-locks = MUST/intersection - same plumbing, opposite meet).
- Concurrent B-tree / hand-over-hand locking track - the shadow-map cases are the
  ones that matter there.
- Shared shadow infrastructure is ~80% of what a leak detector and a lifetime
  histogram ("this unique lived 3us") need - build once, seed two more tools.
