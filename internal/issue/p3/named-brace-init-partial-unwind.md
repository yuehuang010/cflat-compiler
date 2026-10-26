# Named brace initialization move tracking and partial unwind (T470)

**RULED 2026-10-24 (main session; maintainer may veto):** item 1 - keep the conservative behaviour: `T x = move *p;` consumes the pointee and a later read through `p` is refused (safe; clang keeps `p` usable but CFlat has no valid moved-from state for a relocated value). Only the diagnostic text changes: "pointee of 'p' was moved" instead of "dereference of moved variable 'p'" (p itself is not moved).

## Item 1: raw pointer pointee move

Master accepts `T x = move *p;` and `{ h = move *p }` when there is no later read. Both forms reject a later
`p->m.value` with `dereference of moved variable 'p'`; clang++ accepts both and keeps `p` usable. The plain
form has the same rejection, so this is the ruling question specified in the brief. No source change made.

## Item 2: replacement unwind

Top-level and bare-brace field replacement are fixed. The throwing copy case now destroys the replaced `TC`
and sibling `Counted` exactly once (live=0, dtors=3), matching the balanced C++ result. The non-throwing
control remains balanced.

Nested replacement remains open. `Outer.u.t` with a throwing copy still reports live=-1 and TC live=-1,
with six total dtors and three TC dtors versus clang++'s four dtors and one TC dtor. The nested non-throwing
control is balanced. The focused repro is `scratch/t470/c14_nested_replacement_throw.cb` and its full counters
are in `scratch/t470/run_post2.summary.txt`. Three implementation attempts left the nested throw at the same
double-destroy result. The confirmed root-cause boundary is the nested aggregate unwind handoff: the inner
`T470Inner.t` replacement is forwarded through the outer `T470Outer.u` field destination, while the partial
unwind state is suspended and restored by aggregate slot. The current tracking sees the nested destination at
the caller (`MainListener_Aggregates.cpp:320-343`) but still restores ownership at the wrong aggregate level
for this throwing replacement. The attempt-specific strategy notes were not retained; no stronger claim about
which variant failed is recorded. Stop rule reached. The c14 runtime leg was removed from
`Test/test_cpp_interop.cb` because master has the same defect; c18 remains as the nested non-throwing control.

## Item 3: completed call argument

Fixed for named and positional brace arguments. On master, the later-argument throw leaves packed ownership
state 1100; post-fix it is 1111, matching clang++. No-throw controls remain 1111. The regression legs are
in `Test/test_cpp_interop.cb` near lines 18516-18530.

The full `test_cpp_interop` gate still reports HeapAudit allocations (two reports in the suite run, five
in a standalone rerun). Focused c19/c20 probes with the same named/positional ownership shape report zero
heap deltas; the integrated leak source is not isolated and is not attributed to an existing failure.
The other `EmitConstructedFieldOverrides` callers build default-parameter prologues or local initializers;
only the postfix call-argument caller needs to retain its unwind slot across later arguments.

## Left after T470 (review 1, same on master)
- Heap `RF* v = new RF { t = s };` with a throwing replacement copy: the sibling Counted is never destroyed (live=1) and the RF heap
  block is not freed (HeapAudit +1); clang balances. The constructed-object path (MainListener_Expressions.cpp ~29341) calls
  EmitFieldInitializer(typedPtr, typeName, initList) with no completed-object slot and no unwind entries. Fix direction: as
  EmitConstructedFieldOverrides - open an UnwindPartialScope holding the heap entry (UserNewHeap / CflatHeap, ~29331) plus a Slot
  for typedPtr, and pass typedPtr / &typeName as the completed-object slot. Repro scratch/repro_keep/box1022/t470/rev_t470_heap_new_repro.cb.


## Non-throwing leak (T470 Opus r1, PRE-EXISTING, filed 2026-10-24)
Summary: Named brace initialization with a C++ class field leaks one `Counted` value on the non-throwing path.
Repro: `{ c = move cc, t = s }` with a C++ class field from T470 r1 opus review.
Master vs expected: master leaks one `Counted`; expected balanced destruction.
Found: T470 r1 opus review, 2026-10-24 box, PRE-EXISTING
