# A temp's `unique` field still escapes through an INDIRECT callee, or a return the walk cannot follow

Filed 2026-08-08 by `fix/temp-uniq-plain-param`, which closed the plain-`T*` parameter for every
callee whose body PROVABLY stores the pointer into memory that outlives the call. Trimmed
2026-08-10 by `fix/retwalk`, which closed the RETURN-VALUE sub-case (a `return <param>` walk that
re-ledgers the call result, so the existing escape guards fire on a laundered value). Trimmed again
2026-08-10 by `fix/selglob`, which closed the JOINED-STORE sub-case (`MemoryOutlivesCall` now walks
a select/phi destination's arms, ALL-arms). What is left here is the shape that guard's
callee-side fact still cannot answer, plus the residues of the return walk.

Severity: **silent use-after-free**. Compiles clean, runs, exits 0.

## There is no callee to ask - function pointer and interface dispatch

`RecordTempUniqueFieldArgs` reads `CallInst::getCalledFunction()`, and both callee-side walks
(`OwningPtrProvablyEscapes`, `ParameterMayReachReturn`) treat a null callee as "no proof". So an
indirect call accepts:

```cflat
function<void(Node*)> f = keep;   // void keep(Node* n) { g = n; }
f(makeBox().t);                   // measured: dtors=1, dangles

ITake it = keeperC;               // class KeeperC : ITake { void take(Node* n) { this.p = n; } }
it.take(makeBox().t);             // measured: dtors=1, dangles
```

`scratch/tup_a11_fnptr.cb` and `scratch/tup_a12_iface.cb` in the original fix worktree. The
RETURN-value spelling of the same hole was re-measured by `fix/retwalk` and behaves identically -
`function<Node*(Node*)> f = passthru; Node* b = f(makeBox().t);` and the interface twin
`Node* b = ip.go(makeBox().t);` both give `v=99 same=1 dtors=1` before and after that fix
(`scratch/rw/c2_funcptr.cb`, `scratch/rw/c3_iface.cb`). Both are recorded as ACCEPT cells
deliberately: unknown-accepts is the guard family's chosen polarity and these are the honest
unknowns.

The interface half is the more closable of the two - the set of implementors IS known at end of
module (`interfaceTable`), so an interface method could be judged as "every implementor of this
slot provably stores" (or "any implementor may return the parameter"), which is the same shape
`ResolveMaterializedInterfaceUses` already runs at. A `function<T>` value has no such closed world
short of a points-to analysis.

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
"no known callee" and the residues of the return walk itself (callee below the call site,
field-indirect return, budget). Fixing any one alone is a self-contained change. Do not
consolidate them further.

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
