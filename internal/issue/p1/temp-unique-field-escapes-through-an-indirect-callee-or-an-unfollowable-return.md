# A temp's `unique` field still escapes through an INDIRECT callee, or a return the walk cannot follow

Filed 2026-08-08 by `fix/temp-uniq-plain-param`, which closed the plain-`T*` parameter for every
callee whose body PROVABLY stores the pointer into memory that outlives the call. Trimmed
2026-08-10 by `fix/retwalk`, which closed the RETURN-VALUE sub-case (a `return <param>` walk that
re-ledgers the call result, so the existing escape guards fire on a laundered value). Trimmed again
2026-08-10 by `fix/selglob`, which closed the JOINED-STORE sub-case (`MemoryOutlivesCall` now walks
a select/phi destination's arms, ALL-arms). Trimmed again 2026-08-10 by `fix/iface-escape`, which
closed the INTERFACE-DISPATCH sub-case (see below). What is left here is the shape that guard's
callee-side fact still cannot answer, plus the residues of the return walk.

Severity: **silent use-after-free**. Compiles clean, runs, exits 0.

## There is no callee to ask - function pointer dispatch

`RecordTempUniqueFieldArgs` reads `CallInst::getCalledFunction()`, and both callee-side walks
(`OwningPtrProvablyEscapes`, `ParameterMayReachReturn`) treat a null callee as "no proof". So an
indirect call accepts:

```cflat
function<void(Node*)> f = keep;   // void keep(Node* n) { g = n; }
f(makeBox().t);                   // measured: dtors=1, dangles

function<Node*(Node*)> g2 = passthru;
Node* b = g2(makeBox().t);        // measured: v=99 same=1 dtors=1
```

`scratch/tup_a11_fnptr.cb` and `scratch/rw/c2_funcptr.cb` in the original fix worktrees. Recorded
as ACCEPT cells deliberately: unknown-accepts is the guard family's chosen polarity, and a
`function<T>` value has no closed world short of a points-to analysis.

## CLOSED 2026-08-10 by `fix/iface-escape`: the interface-dispatch twin

The interface half WAS the closable one, and is now closed. `CallInterfaceMethod` records the
virtual call (`RecordTempUniqueFieldInterfaceArgs`) and the slot is judged against the closed
implementor set (`EnumerateInterfaceImplementors`, the registry
`InterfaceConversionIsProvablyImpossible` already uses), at the call site when every implementor
body is emitted and otherwise at the end-of-module resolve.

Both directions are ALL-of-implementors, not ANY. The store side must be, or the message ("the
callee stores this pointer") is false on a dispatch to a non-storing implementor. The RETURN side
was given the same polarity for the same reason and against `ParameterMayReachReturn`'s own MAY
shape: the re-ledger feeds a REJECTION, and with one implementor returning something else the
resulting "the result IS the temp's unique field" is false on that dispatch. One counter-example
- a non-storing implementor, an implementor with no readable body yet, an uncertain implementor
set (a generic interface base clause, still inside an import) - accepts.

Evidence: `it.take(makeBox().t)` and `Node* b = ip.go(makeBox().t)` are both rejected; the mixed
two-implementor shape and the read-only implementor still compile and free once. Reject legs in
`Test/errors/err_unique_borrow_into_field.cb` (+ `err_temp_unique_field_iface_deferred.cb` for the
implementor emitted below the call site), accept legs in `Test/test_move.cb`. Corpus in
`scratch/ife_*.cb`.

Residue, deliberately left accepting: a GENERIC interface. A generic base clause records the
template as uncertain (`RecordUncertainInterfaceImpl`, MainListener.h) because the instance name
is not known before substitution, so the implementor set cannot be enumerated and the guard
answers "no proof". Measured: `scratch/ife_s9_generic.cb`, `scratch/ife_s9b_generic_concrete_param.cb`.

## Also left open by `fix/retwalk`: a laundering callee defined BELOW its call site

`Node* b = laterPassthru(makeBox().t);` with `laterPassthru` defined after `main` still compiles
and dangles (`v=99 same=1 dtors=1`, `scratch/rw/b7_callee_after.cb`, measured identical before and
after `fix/retwalk`). cflat emits IR as it walks, so the callee is a bare declaration when the
call is emitted and `ParameterMayReachReturn` correctly reports "no proof". The STORE half of the
same guard copes with this by recording the call and re-asking at end of module
(`ResolveTempUniqueFieldArgEscapes`); the RETURN half cannot reuse that, because the fact it needs
is not "did the callee store" but "where did the RESULT get bound", which is known only at the
declaration/assignment/return site the walk has already passed. Closing it means deferring the
escape SITE, not the callee question - a second record-then-resolve list keyed on the destination.

## Also left open by `fix/retwalk`: the parameter returned back out of a local FIELD

`ParameterMayReachReturn` does not follow a load whose pointer is a GEP, so a callee that parks
the parameter in a local aggregate and returns it out of that FIELD still launders it:

```cflat
struct Slot { Node* q = nullptr; };
Node* viaField(Node* n) { Slot s = default; s.q = n; return s.q; }
Node* b = viaField(makeBox().t);   // ACCEPTED, dangles: v=99 dtors=1, identical before/after
```

Measured on both binaries (reviewer probe, 2026-08-10). The restriction is load-bearing, not an
oversight: relaxing the load rule to follow a GEP was BUILT and measured, and it false-rejects
`int readResourceId(Resource* r) { return r->id; }` in `Test/test_move.cb` itself, because
`r->id` is the same shape (a load off a GEP of the tracked value). A type-based refinement -
follow only a load of POINTER type - false-rejects `Node* getNext(Node* n) { return n->next; }`,
which is correct code. Closing this needs field/offset awareness the walk does not have.

Two budget escapes answer accept the same way and are also unclosed: a launder chain deeper than
`kMaxRetainDepth` (measured: 12 nested passthru callees compile and dangle) and a callee body
whose walk exceeds `kMaxRetainUses` values.

## Why these are one file

They are separate mechanisms, filed together only because they are the remaining complement of one
guard: what the callee-side walks answer is "a KNOWN callee that STORES or RETURNS", and these are
"no known callee" (now only `function<T>`) and the residues of the return walk itself (callee
below the call site, field-indirect return, budget). Fixing any one alone is a self-contained
change - the interface half was fixed exactly that way. Do not consolidate them further.

## Also known, and deliberately accepted

`OwningPtrProvablyEscapes` follows a load back out of a stack slot only while every store into
that slot is a value the walk already tracks, so `Node* p = n; if (c) { p = other; } g = p;`
proves nothing and accepts. That is the safe direction and is not a filed bug.

The DESTINATION walk answers the mirror-image shape the other way, and did so before `fix/selglob`:
`SlotHoldsOutlivingPointer` proves on ANY store into the address slot, so
`Node** p = &loc; if (c) { p = &g; } *p = n;` is REJECTED while its select spelling
`Node** p = c > 0 ? &loc : &g;` is accepted (both measured on `a4167aa` and after) - but only
when no arm is itself a slot LOAD. `fix/selglob`'s join rule is ALL-of-ANY: every arm must prove
outliving, yet an arm that is a load of a slot is proven by the slot's ANY-store rule, so a select
over a mixed slot (`q = c > 0 ? p : &g2` where `p` held `&loc` then conditionally `&g`) rejects
even though one path escapes nothing (safe direction, measured flip on `fix/selglob`). The two
polarities are deliberate - the slot rule is the older, blunter one - but they disagree on the
same program written two ways. Reconciling them is unfiled work, not a regression.

Related: [[interface-issue-queue]]
