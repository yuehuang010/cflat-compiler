# `o = w.b` silently consumes a BORROWED pointer parameter's field, while `o = move w.b` rejects it

Filed 2026-08-09 by the review of `fix/owncopy` (the indirect owning-SOURCE fix). Not a
regression of that fix: the deref-DESTINATION arm already behaved this way before it, and the
widening only extends the same behaviour to the plain `=` and decl-init spellings.

Prelude: `Res` with a dtor counter, `Box { unique Res* item; }`, `Wrap { Box b; }`, `makeBox`.

## The asymmetry

```cflat
void takes(Wrap* w)
{
    Box o = makeBox(1);
    o = move w.b;   // COMPILE ERROR: "cannot 'move' field 'Wrap.b' through borrowed parameter
                    // 'w'. Declare the source parameter 'move w' to transfer ownership ..."
    o = w.b;        // ACCEPTED, and does exactly what the rejected spelling asked for:
                    // the CALLER's w.b.item ends up nullptr, no diagnostic
}
```

Measured (`scratch/rev_p_ptrparamfield.cb`, `rev_p_movethruptr.cb`): the implicit form runs
rc 0 with one free per allocation (memory-safe - before `fix/owncopy` it was rc 133), and the
caller's field reads null afterwards. The explicit form does not compile.

The pre-existing precedent: the deref-DESTINATION arm (`*dp = w.b`, `MainListener_Expressions.cpp`
~2168) already consumed a borrowed-pointer parameter's field silently on the pre-fix binary
(`scratch/rev_p_derefdst_parambfield.cb`, rc 0 / caller nulled on BOTH binaries).

## Scope of the borrow guard

Only the POINTER-parameter spelling is guarded. `move` out of a `T[]` VIEW parameter's element
(`o = move v[0]` over the caller's array) and out of `this.field` inside a method both compile
and consume today, on both binaries - so their implicit twins are consistent with the explicit
ones, and only the pointer-parameter case diverges.

## Fix direction

Pick ONE reading and apply it to both spellings:

- extend the borrowed-parameter guard to the implicit store (a source lvalue whose address roots
  at a borrowed pointer parameter is not consumable - reject with the existing message), which
  also has to cover the deref-DESTINATION arm; or
- drop the guard on the explicit `move` and accept that a borrowed pointer is a writable lvalue,
  matching the view-parameter and `this.field` cases.

Do NOT simply narrow the `fix/owncopy` arm back: the sources it consumes here used to DOUBLE-FREE
(rc 133), so a plain narrowing trades a silent value loss for memory unsafety.

## Deliberately left open by `fix/bvfield` (2026-08-09)

`fix/bvfield` closed the BY-VALUE twin of this issue (that file is deleted) by rejecting every
implicit consume whose source field path roots at a non-`move` by-value struct parameter. It did
NOT extend that guard to the pointer-parameter spelling here, and the reason is a measurement:
`Test/test_move.cb`'s `bps_ptrfieldsrc_*` legs and the new `cbvf_ptrparam_*` legs pin the pointer
spelling GREEN - it compiles, nulls the caller's field, and frees once (rc 0). Rejecting it would
turn a program the compiler runs correctly into a hard error, which is a BEHAVIOUR CHANGE to
working code and a maintainer design decision, not a bug fix.

The two spellings are therefore no longer symmetric, and that asymmetry is now the visible face of
this issue: `int f(Wrap w) { UBox o = w.b; }` is rejected, `int f(Wrap* w) { UBox o = w.b; }` is
accepted and silently takes the caller's value. Whichever ruling is picked, it has to be applied to
the explicit `move w.b` spelling in the same change - that one is already rejected here.

Note for whoever takes it: the by-value guard answers the question against the RESOLVED binding
(`NamedVariable::RootIsBorrowedByValueParam`, settled at the field-access site), not against the
root's NAME. A pointer-parameter version must do the same - a name-only test false-rejects an inner
local that shadows the parameter, which is exactly what the review of `fix/bvfield` caught.

### A worse neighbour: `&w` on a BY-VALUE parameter (measured, still open)

Taking the address of the by-value parameter launders the field consume past the new guard, and
unlike the pointer-PARAMETER spelling above it is not memory-safe:

```cflat
int f(Wrap w) { Wrap* p = &w; UBox o = p->b; return o.item->id; }   // scratch/rev_45
```

-> compiles 0, prints `v=3`, then ABORTS (rc 133). Measured identical on `6c2302c`, on
`fix/bvfield`'s first round, and on its final commit - so it is pre-existing and untouched, not a
regression. The guard structurally cannot see it: the field path roots at the pointer LOCAL `p`,
not at `w`, so `RootIsBorrowedByValueParam` is false. Whichever ruling is picked for this issue has
to cover the address-of spelling too, since it is the double-freeing member of the family.

## Related

`internal/plan/ownership-transparent-assignment.md` (`=` is total over T); the landed
`fix/owncopy` record at the bottom of `internal/fix-issue-lessons.md`.

## Update 2026-08-10 - the BY-VALUE parameter half is now closed for `unique` fields

`fix/uniq-implicit-move` extended `RejectConsumeOfBorrowedByValueParamField` to the implicit-move
field-store path (both the `=` and the brace-init site), so `gDest.slot = h.slot` inside
`void sink(Holder h)` now rejects with the same reason `move h.slot` always did - the two spellings
agree. Legs in `Test/errors/err_unique_borrow_into_field.cb`; the named remedy (`move Holder h`) is
measured working. This file's asymmetry is unchanged: it is about the POINTER-parameter spelling
(`w->b`) and about owning-VALUE fields, neither of which that guard reaches.
