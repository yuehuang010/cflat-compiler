# `unique` assignment: owning value laundered through a BORROW-returning call still leaks

## RULING 2026-08-10 (maintainer) - BUILD the alias proof, AND take the two precision gaps.

**1. The return-identity alias proof.** Prove that EVERY return of the callee is EXACTLY the tracked
argument, with no other escape - then the owning destination adopts the result and the temp stops
leaking. This is the "only direction that closes this soundly" named below, and it is the strictly
stronger form of `ParameterMayReachReturn`, which already lives beside `ParameterRetainsArgument` in
`cflat/LLVMBackend_OwnershipTemps.cpp`. An extension of an existing walk, not a new analysis.

Caveat: cflat emits IR as it walks, so a callee defined BELOW its call site yields no proof and
keeps leaking. That is the unknown-accepts polarity and is acceptable; a record-then-resolve pass at
end of module would shrink it, as `ResolveTempUniqueFieldArgEscapes` already does for a sibling
guard.

**2. Both "deferred gap - escape-analysis precision" shapes are IN SCOPE** - the projected pointer
handed to an unanalyzable callee (`printf("%s", tag.data())`) and the field assignment that runs the
field's destructor through `this` (`tag = s.copy()`). Both need a stronger question than the current
one: **"does not retain PAST my call"** rather than "does not escape the callee". This is materially
larger than item 1 and CHANGES WHEN DESTRUCTORS RUN, so it needs its own differential corpus sweep.
Do not weaken the existing answers to buy the precision back - build the stronger question.

**3. Still deferred:** the `llvm.mem*` DESTINATION analog. Unreachable today (a whole-struct field
assignment lowers to `store %Inner`, verified on emitted IR); becomes live only if struct assignment
ever lowers to a memcpy.

**Unchanged prohibition:** do NOT loosen `AsDirectNew` / `TopLevelMoveExpression` to look through
wrappers - that reintroduces the `b = addr(new R());` double free. Ask whether the RHS's resulting
VALUE carries ownership, not what the RHS looks like.

Follows the 2026-08-10 governing principle - "CFlat is a simplified reading: if ownership can be
computed, it should be implicit" - stated in full in
[[unique-field-to-field-array-element-receiver]]. `self()` visibly returns `this` unchanged, so the
ownership IS computable and should not require the user to restructure the expression.

Filed 2026-07-24. Last narrowed 2026-07-26.

## Residual - owning call laundered through a self-returning method (LEAKS)

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); }  R* self() { return this; } };
move R* make() { return new R(); }
extern int main()
{
    unique R* b = nullptr;
    b = make()->self();                // owning temp from make() is never freed - LEAKS
    return 0;
}
```

Two independent things have to be true to free this temp, and neither is:

1. The RECEIVER cannot be released at the statement boundary, because `self()` RETURNS it -
   `ParameterRetainsArgument` answers "retains" for argument 0, correctly. Releasing it would
   destroy the pointee `b` now names, i.e. a use-after-free rather than a leak.
2. `b` cannot ADOPT the call's result, because that result is a DIFFERENT SSA value from
   `make()`'s owning result, so no value-identity ledger sees it as owning. Adopting a
   borrow-returning call's result is only sound with an alias proof that the result IS the owning
   temp AND that the callee retains it nowhere else; nothing proves that today, and adopting it
   unconditionally would double-free (the temp is separately eligible for `pendingOwnedPtrTemps`
   cleanup).

So the object is owned by nobody: `b` holds a borrow it never adopted, and scope exit frees
nothing. Harm is a LEAK, and a leak is the accepted trade; a double free is not. Leave it leaking
unless an alias proof (a callee whose every return is EXACTLY the tracked argument, with no other
escape) is built - that is the only direction that closes this soundly. Pinned as-is by
`recv_temp_escaping_*` in `Test/test_move.cb`, including a leg that dispatches through `b` after
the assignment to prove the value is still live.

Do NOT fix this by loosening `AsDirectNew` / `TopLevelMoveExpression` to look through wrappers -
that reintroduces the `b = addr(new R());` double free. Ask whether the RHS's resulting VALUE
carries ownership, not what the RHS looks like.

## Deferred gap - escape-analysis precision (LEAKS, sound)

`OwningPtrEscapes` answers "retains" - so the owning temp leaks instead of being released at the
statement boundary - for two shapes where a free would in fact be safe:

- **A projected pointer handed to an unanalyzable callee.** `void show() { printf("%s",
  tag.data()); }` on an owning receiver temp: `data()` returns the buffer pointer (an escape from
  `data`), and `printf` is a varargs declaration, which always answers "retains". Nothing can
  outlive the statement here, but proving it needs "does not retain PAST MY call" rather than
  "does not escape the callee".
- **A field assignment that runs the field's destructor through `this`.** `void setTag(string s)
  { tag = s.copy(); }` emits `call void @string.dtor(ptr <gep of this>)` before the store. A callee
  that frees through the tracked pointer answers "retains" (otherwise a caller-side release would
  double-free), and that rule cannot currently tell "freed the object" from "freed one field the
  callee then overwrote".

Both are leaks, never use-after-frees. Recovering them needs a stronger question than the current
one; do not weaken the existing answers to buy the precision back.

## Deferred gap - the `llvm.mem*` DESTINATION analog (latent)

The store-through rule (`StoredValueMayBeCallerOwned`) only sees `store` instructions, and the
`llvm.mem*` rule only rejects the tracked pointer as the memcpy SOURCE. A memcpy whose
DESTINATION is the tracked pointer and whose source holds a caller-owned pointer would park that
pointer inside the pointee without any rule firing. It is unreachable today: a whole-struct field
assignment lowers to `store %Inner` (verified on the emitted IR), so the store rule catches it. It
becomes live if struct assignment ever lowers to a memcpy - add the destination analog then.

## What IS covered

- `b = new R();`, `b = move a;`, `b = makeOwned();`, `b = (R*)make();` - adopted.
- `b = cond ? new R() : nullptr;` and `b = cond ? new R() : new R();` - adopted.
- `b = addr(new R());` - correctly NOT adopted (the `new` is an ARGUMENT; the RHS result value is
  the borrow-returning call's own result). Also pinned for the ternary-in-argument shape
  (`borrowFirstResource(t3, useNew ? new Resource() : nullptr)`).
- An owning temp used as a METHOD-CALL RECEIVER is judged by the same escape analysis as an
  argument (`this` IS argument 0). It is released at end-of-full-expression when the callee only
  reads scalars out of it, reads a pointer-bearing field without handing it out (`tag.length()`),
  writes scalars into it, or parks a `move` parameter or a fresh allocation into it. It is left
  alone (leaked) when the callee returns it, hands out anything read out of it (a pointer, a
  `string`, a union, or a by-value snapshot), or parks a caller-owned pointer into it.
