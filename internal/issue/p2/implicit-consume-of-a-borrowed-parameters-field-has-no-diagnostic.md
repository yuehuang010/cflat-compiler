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

## Related

`internal/plan/ownership-transparent-assignment.md` (`=` is total over T); the landed
`fix/owncopy` record at the bottom of `internal/fix-issue-lessons.md`.
