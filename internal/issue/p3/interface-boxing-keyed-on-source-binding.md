# `RegisterInterfaceBox` dedupes on `FatValue` only (preventive)

NARROWED AGAIN 2026-08-02. The live defect this file carried - `delete` of an interface box whose
object a different owner already frees is accepted and double-frees - is CLOSED. Only the
preventive remainder below is left. The FILENAME and path are kept unchanged so the
`[[interface-boxing-keyed-on-source-binding]]` links from [[interface-issue-queue]] and the related
issues still resolve; the title no longer describes a live double free, and this is no longer a P1.

## What was closed (2026-08-02)

`delete <interface value>` is rejected when the boxing site can PROVE that a different owner frees
the object. Eight spellings reached the double free and all eight now diagnose: a borrowed pointer
PARAMETER, the assignment statement, a parenthesized source, an `as` cast, a box created inside a
GENERIC, a `unique` FIELD read, the `?:` join and the `??` join. Regression legs:
`Test/errors/err_delete_borrowed_interface_box.cb` (one leg per spelling, each pinning its own
local's name, each mutation-tested individually) and the `delete_box_*` / `delete_borrowed_box_*`
legs in `Test/test_move.cb` (the must-still-work half, which asserts values and free counts).

### The proof, and the negative that must never be used as one

The rejection needs a POSITIVE proof of a NAMEABLE other owner, taken at the BOXING site while the
source binding is still in hand. Seven things count, in `BindingKeepsOwnershipOfBoxedObject`: the
binding frees the object itself at scope exit (`IsOwning`); it reads a `unique` FIELD, whose
synthesized destructor frees it; it borrows a CONTAINER's element; a proof PROPAGATED across an
assignment from the RHS binding (`InheritedKeepsOwner`); it aliases a borrowed parameter
(`IsBorrowed` with a named `BorrowedOrigin`); it is a plain COPY of a still-live owning local
(`BorrowsOwningLocal`, added by fix/untracked-copy); or it is a non-move pointer PARAMETER. It is recorded
on the ledger as `InterfaceBoxRecord::SourceKeepsOwner`.

`IsAliasBorrow` is excluded, measured false-rejecting a program master compiles and runs correctly:
it is the OPPOSITE of this question. It means the binding's own scope-exit free is SUPPRESSED - it
frees NOTHING. `alias` hands the lifetime to the receiver to manage by hand, so
`alias T* e = makeT(); IS s = e; delete s;` is the CORRECT way to release it. Rejecting it both
false-rejected and leaked, and contradicted the compiler itself, which accepts the raw `delete e;`
for the same binding.

### Retirement, propagation, and why the clause ORDER is load-bearing

Facts that can go stale are retired by `MarkPointerRebound` on a plain `=` into a pointer binding:
"this is a borrowed parameter" is true of the DECLARATION and false once the binding points
elsewhere. `int g(T* p) { p = new T(); ... }` makes the frame the sole owner, and both the
shadowed-name and the reassigned-parameter forms were false-rejected before that retirement existed.

Retirement is NOT unconditional, and it is not the only thing that store does. Three corrections,
each measured against a binary built at the first cut of this guard:

- **`??=` is a JOIN and takes the JOIN rule, not either side alone.** Its handler `return`s before
  the `operatorText == "="` block, so `int f(Ci* p) { p ??= new Ci(); IS s = p; delete s; }` kept
  its declaration-time borrowed-parameter fact and was FALSE-REJECTED - master runs it correctly at
  dtor=1 - and the diagnostic's own remedy (drop the delete) ran to dtor=0, a LEAK. Afterwards the
  binding holds either its OLD referent (arm not taken) or the RHS's (arm taken), so the rule
  mirrors the one for the `?:` / `??` joins:
  - **Both sides prove** -> propagate, with the two owners rendered as a join
    (`'p' or 'q'`, `its container or 'q'`). Retiring here instead laundered `p ??= q` between two
    borrowed parameters into a double free that the raw `delete p;` on the same binding rejects.
  - **Either side proves nothing** -> provenance unknown -> plain retirement.

  "Proves" on the RHS is bounded by what proof RECOVERY can see: a load off a live binding's
  alloca, on both the `=` and `??=` paths. A FIELD-read RHS is a load off a GEP and proves
  nothing, even a `unique` field - see the accepted-gaps list below.

  Taking the RHS alone was considered and rejected: for `Ci* c = nullptr; c = new Ci(); c ??= q;`
  the not-taken arm leaves `c` the SOLE owner (`IsOwning` is decl-with-init only, so `c` never
  scope-exit-frees and the box's delete is the only free), so blaming `q` would be a false rejection
  with a leaking remedy - the failure class this branch died on. `delete_box_coalesce_sole_owner_*`
  in `Test/test_move.cb` is that leg.

  Because the handler returns early, the `??=` store is also marked `CoalesceRebound`, which
  suppresses the ELEMENT clause of this proof only: `SetVariableBorrowsOwnedElement` never runs for
  `??=`, so the declaration's element fact may be stale, and it outranks the retirement by design.
  Without that, `l.add(nullptr); T* e = l.get(0); e ??= new T(); IS s = e; delete s;` was
  false-rejected with a leaking remedy while master runs it at dtor=1. The RAW-delete guard reads
  `BorrowsOwnedElement` directly and is deliberately NOT gated - clearing the taint outright also
  widened the raw guard, a behaviour change master does not have. The rest of the bookkeeping that
  early return skips is [[coalesce-assign-skips-store-bookkeeping]].
- **A proving RHS PROPAGATES instead of retiring.** `int f(Ci* p, Ci* q) { p = q; IS s = p;
  delete s; }` leaves `p` a borrow, yet the store cleared the fact and the program compiled clean and
  aborted (134); deleting the `p = q;` line made it reject. `MarkPointerRebound` now takes the RHS
  binding's own rendered owner - recovered by STORAGE IDENTITY, never by spelling, so `p = q->next`
  cannot resolve to its base object - and sets `InheritedKeepsOwner` when it proves. Empty is the
  accept direction, so every shape that does not resolve retires exactly as before.
- **The clauses a store REFRESHES outrank the bit that store sets.** `BorrowsOwnedElement` was asked
  BELOW `PointerRebound`, while `SetVariableBorrowsOwnedElement` re-establishes the element taint on
  that very same `=`. So `T* g = nullptr; g = l.get(0); IS s = g; delete s;` had a fact fresher than
  the bit retiring it, and the stale bit won: accepted, exit 134. `BorrowsOwnedElement` and
  `InheritedKeepsOwner` are now asked ABOVE the retirement; `IsBorrowed` and the parameter test,
  which are DECLARATION-time, stay below it. `Test/test_move.cb`'s `delete_box_hop_reassigned_*` and
  `delete_box_elem_reassigned_*` legs pin the accept side of exactly that ordering - moving
  `IsBorrowed` above the retirement makes `Test/test_move.cb` fail to compile.

**`IsBorrowed` was briefly dropped from the proof** because it survives a reassignment that makes
the local a sole owner (`T* b = p; b = new T();`). Dropping it opened a LAUNDERING PATH: for
`int f(Ci* p) { Ci* b = p; ... }` the compiler REJECTED `delete b;` and ACCEPTED `IS s = b;
delete s;` on the identical binding, and the accepted form aborted (134); same for a one-hop copy of
a `unique` field. It is restored, asked BELOW the retirement - which is what the reassignment case
actually needs - and gated on `!BorrowedOrigin.empty()`, the same pair of conditions the raw-delete
guard uses, so the boxed and raw spellings now reject exactly the same set. Measured, not inferred:
the reassignment legs behave identically on the pre-restore and post-restore binaries.

The parameter test is by STORAGE IDENTITY, never by spelling: `IsFunctionParameter` is a name-only
scan of every live frame, so a local SHADOWING a parameter's name was classified as that parameter
and its solely-owned allocation was blamed on the caller.

**`OwnershipTransferred == false` is NOT a proof and an earlier cut used it as one.** It is also
false for a pointer that received its `new` in a LATER statement (`T* c = nullptr; c = new T();`),
because `IsOwning` is set at declaration-with-initializer only, so there was never anything for
`RetireOwningSourceOfBoxedValue` to retire. Such a local is the box's ONLY owner, so deleting the
box is correct - and the diagnostic's "let the source release it" advice LEAKED there, freeing
nothing. Four natural spellings (late assignment, the assignment-statement box, assignment in a
branch from a factory, assignment in a loop) were all false-rejected. A 513-file corpus sweep found
this defect NOT AT ALL, because no in-repo `.cb` boxes a late-assigned pointer and deletes it; only
a targeted acquisition-axis corpus found it.

A field read is asked about the FIELD, never about the enclosing binding: a plain `T* h` field read
through a parameter was briefly blamed on the parameter, false-rejecting a boxed
`delete param->field`. A plain field is ACCEPTED because nothing proves another owner - NOT because
it is safe: a holder whose hand-written destructor frees the same field still double-frees, exactly
as on master, and that stays an open gap. `unique` carries the proof and is rejected.

### The two flags, and which one is sticky

`BorrowedInterfaceBox` is NOT sticky - any later not-proven binding clears it, which is what keeps
`IShapeMove s = p; s = new SqMove(); delete s;` compiling. Only `InterfaceBoxProvenanceUnknown` is
sticky, and only in the ACCEPT direction. The cost of that stickiness is exactly one shape:
`IShapeB s = new Ci(); if (k) { s = p; } delete s;` stays accepted. That is deliberate - walk order
over the AST is not control flow, so a binding seen earlier in the text may not be the one that
reaches the delete - and it is the price of not false-rejecting the reverse order.

### Deliberately accepted, and not regressions

- `T* c = new T(); T* b = c; IS s = b; delete s;` and the `alias T* b = c;` spelling were left
  double-freeing here, and are now CLOSED - see the fix/untracked-copy design record in
  [[interface-issue-queue]]. The aliasing is recorded at the DECLARATION as its own flag pair
  (`BorrowsOwningLocal` + the source's slot), not by widening `IsBorrowed`, and it is a SEVENTH
  clause of the proof below, asked with the declaration-time ones. It retires at BOTH ends: the
  copy rebound, or the SOURCE rebound. The copy off a PARAMETER is still the separate case it was.
- `IShapeB s = new Ci(); delete s; s = p; delete s;` - see the stickiness note above.
- A plain `T* h` field whose holder has a HAND-WRITTEN destructor freeing it. Identical on master;
  no proof is available at the boxing site, since a plain field carries no ownership marker.
- `T* g = new T(); g = l.get(0); IS s = g; delete s;` - an element borrow assigned over an
  ALREADY-OWNING local. `g` keeps a stale `IsOwning`, so the box takes ownership transfer from it
  and the container frees the element too. Out of scope here: the RAW `delete g;` spelling
  double-frees identically, on master and on this branch, so the defect is in the ownership
  bookkeeping the raw guard reads, not in the boxing proof. The `nullptr`-initialized spelling of
  the same program IS rejected (`borrowElemAssign`).

- GLOBAL bindings are outside the guard entirely - both a global pointer as the assignment LHS or
  RHS, and a global interface RECEIVER (`IS gs = nullptr; gs = p; delete gs;`). Every clause is
  gated on `AllocaInst` storage and every lookup scans `stackNamedVariable` only, so a
  `GlobalVariable` binding can never be proven and always lands accept-direction. Measured rc=134 on
  master, on the first cut of this guard, and here - pre-existing, not delta-introduced.
- `T* e = nullptr; e ??= <proving RHS>;` - the null LHS proves nothing, so the join rule retires and
  the shape is accepted. rc=134 on all three binaries; the deliberate cost of not false-rejecting
  the sole-owner-LHS leg above.
- A FIELD-read RHS is not recoverable as a proof, on the `=` path or the `??=` path: recovery
  requires a load off a live binding's ALLOCA (`DescribeAssignedSourceOwner` /
  `ProvingBindingForBoxedSource`), and a field read is a load off a GEP. This includes a `unique`
  field: `int f(Ci* p, HoldU* hh) { p ??= hh->h; IS s = p; delete s; }` runs rc=134 on master, the
  first cut, and here (the raw `delete p;` twin rejects), and the plain-`=` twin `p = hh->h;` misses
  identically. Symmetric, pre-existing, accept-direction - a missed rejection, never a false one.

## Measured asymmetry left in place

`int f(Ci* p) { Ci* b = p; b = new Ci(); delete b; }` is FALSE-REJECTED by the raw-delete guard
("it aliases borrowed parameter 'p'") on master and here, while the boxed spelling of the same
program is correctly accepted. The raw guard does not consult `PointerRebound`. Pinning the boxing
proof to the raw guard's answer would therefore have imported a false rejection; the two agree on
the un-reassigned binding, which is the case that mattered for the laundering path.

## Still open, preventive

`RegisterInterfaceBox` still dedupes on `FatValue` only, so two records sharing BOTH a
`DataPointer` and the same `Source` resolve first-registered-wins. Harmless today: such a pair is
either same-`Source` or impossible (a second box off an owning binding is a hard `use of moved
variable`). Closing it properly means keying the dedupe on `(FatValue, DataPointer, Source)`.

## Related

[[delete-of-untracked-pointer-copy-not-diagnosed]] - the accepted gap above.
[[return-dangle-missed-when-slot-has-extra-user]] - its residue wants the same provenance-based
reasoning the closed half above now uses.
[[nullcoalesce-join-not-boxed-on-return-and-call-arg]] - the unfinished half of the `??` work.
[[interface-issue-queue]]
