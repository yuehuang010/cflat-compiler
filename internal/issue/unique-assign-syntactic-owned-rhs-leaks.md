# `unique` assignment: owning value laundered through a BORROW-returning call still leaks

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
