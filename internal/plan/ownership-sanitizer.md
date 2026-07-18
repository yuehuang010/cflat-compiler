# Ownership Sanitizer (`--sanitize=ownership`)

Status: DESIGN / BRAINSTORM. No implementation. Captures the discussion so it can
be picked up later.

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
   SOUNDNESS FIX, not a sanitizer feature. Tracked as a known issue:
   internal/issue/move-state-loop-merge.md. NOTE the shared machinery with the lock
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

- M0 (do REGARDLESS of the sanitizer): fix the loop back-edge move-state merge. This is
  a language soundness bug today, not a sanitizer feature. See Static/runtime split above
  + internal/issue/move-state-loop-merge.md.
- M1: trap shim + move-origin diagnostics layered ON the null crutch (see "Shadow slot
  vs null crutch" below). Small, shippable, pure UX win - upgrades anonymous null-derefs
  to "moved into y at L40, used at L55". NOT a parallel i8 state machine.
- M2: the shadow map, DESIGNED THREAD-SAFE FROM DAY ONE (per-thread or atomic shadow).
  This is the actual project. Threads are NOT a later milestone: the payoff cases are
  concurrent (the B-tree), so a shadow map that races in the exact scenario it exists for
  is worthless. (Merges the old M3 + M4.)
- Generics fold in wherever they arrive - hooks already fire per-instantiation (Box__int),
  so the old "post-monomorphization ordering" open question is RESOLVED; deleted below.

### Decision that gates whether M1-as-a-check even exists

Use-after-move on LOCALS is Rust's exact problem, and move-state already exists as
compile-time flow-sensitive typestate. A conditionally-moved local
(`if (c) sink(move x); use(x);`) can be REJECTED STATICALLY as "possibly moved" (Rust's
answer: conservative static error, zero runtime cost). Preferring a RUNTIME check there
is a deliberate LANGUAGE-POLICY choice to PERMIT patterns Rust rejects and defer to
runtime. The plan never makes that call - make it FIRST. Recommendation: conservative
static error for locals (matches CFlat's existing "straight-line move-then-use is a
compile error" posture); runtime shadow state ONLY where identity is genuinely dynamic
(the shadow map). The honest justification for runtime shadow state is EXCLUSIVELY at
the shadow-map level: containers, trees, heap, aliasing - where binding-to-object is not
statically knowable.

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

- internal/issue/move-state-loop-merge.md - the M0 static soundness fix (loop back-edge
  move-state merge) that this plan's split depends on.
- internal/plan/lock-hand-over-hand.md - also pointer-variable typestate; the loop-merge
  "possibly-X at a join point" machinery should be built ONCE and shared (move-state =
  MAY/union; held-locks = MUST/intersection - same plumbing, opposite meet).
- Concurrent B-tree / hand-over-hand locking track - the shadow-map cases are the
  ones that matter there.
- Shared shadow infrastructure is ~80% of what a leak detector and a lifetime
  histogram ("this unique lived 3us") need - build once, seed two more tools.
