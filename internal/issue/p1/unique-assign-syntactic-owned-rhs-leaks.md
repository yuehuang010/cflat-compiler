# `unique` assignment laundering: residue after the return-identity alias proof landed

The 2026-08-10 RULING's item 1 (the return-identity alias proof) and the FIELD-OVERWRITE half of
item 2 have LANDED. The remaining shape from item 2 (varargs) received its own ruling later the
same day - the va_arg C-boundary axiom pair below - and is now IMPLEMENTABLE work, not a deferral.
The accepted caveats stand. Item 3 (the `llvm.mem*` DESTINATION analog) stays deferred and
unreachable, exactly as ruled.

## What landed

- `ParameterIsExactlyReturned` (`cflat/LLVMBackend_OwnershipTemps.cpp`): EVERY return of the callee
  is EXACTLY the tracked argument AND the argument escapes nowhere else, so the result and the
  argument name one object. `AdoptLaunderedOwningTempResult` then MOVES the owning-temp ledger
  entry from the argument to the call result (never copies it - two entries are two free sites),
  and the existing value-identity leg of the assignment adopts it. `b = make()->self();` now frees
  exactly once. Pinned by the `recv_temp_escaping_*` / `alias_proof_*` legs in `Test/test_move.cb`.
- `ParameterRetainsArgumentPastCall`: the "does not retain PAST my call" question. Same walk with
  the callee's OWN return not counted as an escape, asked only about a NESTED callee. A pointer
  handed back into my frame is still mine to judge, so the walk follows the call RESULT instead of
  answering "retains".
- `CallIsOverwrittenFieldDestructor`: `void setTag(string s) { tag = s.copy(); }` runs the FIELD's
  destructor through `this` and stores the replacement back over that same address. Proven shape,
  so the receiver temp is released at the statement boundary. A callee that frees the WHOLE object
  through the tracked pointer still answers "retains" - the deallocation runs on the tracked
  pointer's own block, and that is the discriminator.

## Residue - a projected pointer handed to an UNANALYZABLE callee - RULED 2026-08-10

### RULING (maintainer) - a va_arg slot is a C BOUNDARY. Take the axiom pair.

A variadic argument slot is C data: CFlat ownership never transfers INTO one. Two consequences,
both to be implemented together:

1. **A borrow handed to a va_arg slot is never retention.** The variadic callee reads C data; it
   cannot become an owner, so `OwningPtrEscapes` may answer "does not retain past my call" for the
   variadic portion of any call by AXIOM, not by walking the body. The owning receiver temp in the
   `show()` repro below is then released at the statement boundary and the filed leak closes. The
   `free`-family exclusion stays: a deallocating callee frees through its DECLARED (non-variadic)
   parameter, which this axiom does not cover, so "the callee freed the object" keeps answering
   "retains".
2. **An OWNING value into a va_arg slot is a hard error.** Ownership cannot cross the boundary, the
   callee cannot free it, and the caller just dropped its only handle. Measured 2026-08-10
   (`scratch/va_q3.cb`): `printf("%p\n", mkOwned())` with `move R* mkOwned()` compiles with no
   diagnostic and the object is never freed - a silent leak today. The error should direct the user
   to bind the value to an owner first. Accept-set to freeze before the guard: every existing
   `printf`-style call over BORROWED pointers (`%p` of a plain local, `.data()` of an owned-elsewhere
   string, interior pointers) stays legal - only an owning temp / move-returned / explicitly moved
   value entering a variadic slot errors.

Supporting measurement (2026-08-10, `scratch/va_q1.cb` / `va_q2.cb`): `int* a = move data();` is
already a compile error for BOTH borrow-returning and move-returning callees - "'move' expression
requires an addressable source (field or local)" - so ownership at a call site is carried solely by
the return TYPE and is statically visible exactly where guard 2 needs it.

### The repro this ruling closes

```cflat
void show() { printf("[%s]\n", tag.data()); }   // on an owning receiver temp
```

`data()` is now transparent (its parameter escapes only via its return, so the walk keeps following
the buffer pointer in `show`'s frame). The pointer then reaches `printf`, a VARARG callee whose
variadic slots are read back through a `va_list` the walk cannot follow. The previous position -
that answering "does not retain" would be trusting an unanalyzable callee - is superseded by the
boundary axiom above: the question is no longer what the callee does, but what a va_arg slot can
BE. Note the axiom covers only the VARIADIC slots; a pointer passed through a DECLARED parameter of
a variadic function is still judged by the ordinary walk.

Sibling cell, decided: a callee that frees a field WITHOUT overwriting it keeps answering
"retains". No proof of the overwrite, so no caller-side release.

## Accepted caveats (ruled, not bugs)

- **The no-discard check now sees the laundered result.** Moving the ledger entry onto the call
  result means a DISCARDED laundering call is a discarded owning value: `identity(make());` is now
  rejected ("owning return value of 'make' must not be discarded"), where it used to compile and
  leak. `_ = identity(make());` frees it exactly once. The diagnostic names the ORIGINATING
  function, not the laundering one. The raw-`new` sibling `identity(new R());` carries an
  `OwnedNewTemp` entry, which the no-discard check does not read, so it still compiles and leaks -
  the same leak it had before this landed, now inconsistent with the `move`-return spelling.
- **A callee whose RETURN is itself a laundering call yields no proof.**
  `R* f(R* p) { return ident(p); }` - `ReturnedValueIsExactlyArgument` sees a `CallInst`, not the
  argument, and does not recurse into it. Adoption still chains at the CALL SITE
  (`ident(ident(mk()))` frees once), so this only costs the through-a-wrapper-body spelling, which
  keeps leaking (safe polarity).
- **A callee defined BELOW its call site yields no proof and keeps leaking.** cflat emits IR as it
  walks, so the body is not readable when the call is emitted; `FunctionBodyIsComplete` reports
  false and the unknown-accepts polarity keeps the temp alive. A record-then-resolve pass at end of
  module (as `ResolveTempUniqueFieldArgEscapes` already does for a sibling guard) would shrink it.
  Pinned by `alias_proof_callee_below_no_proof_leaks`.
- **The DECLARATION spelling is unchanged.** `unique R* b = make()->self();` is still a hard
  rejection ("cannot initialize unique from a borrowed value"), and a plain `R* p = make()->self();`
  still leaks - the declaration path reads the sticky `lastOwningResult` / `lastCallReturnsOwned`
  channels, which a laundering call has already spent. Only the ASSIGNMENT form was in the ruling.

## Deferred gap - the `llvm.mem*` DESTINATION analog (latent, unchanged)

The store-through rule (`StoredValueMayBeCallerOwned`) only sees `store` instructions, and the
`llvm.mem*` rule only rejects the tracked pointer as the memcpy SOURCE. A memcpy whose DESTINATION
is the tracked pointer and whose source holds a caller-owned pointer would park that pointer inside
the pointee without any rule firing. Unreachable today: a whole-struct field assignment lowers to
`store %Inner` (verified on the emitted IR). Add the destination analog if struct assignment ever
lowers to a memcpy.

## Unchanged prohibition

Do NOT loosen `AsDirectNew` / `TopLevelMoveExpression` to look through wrappers. The alias proof is
a VALUE-identity proof and cannot make the mistake the syntactic route makes: for
`borrowFirstResource(h, new Resource())` the proven-returned parameter is the FIRST one, which is
not an owning temp, and the `new` in the second slot is never the result - pinned by
`unique_reassign_borrow_call_not_adopted`. `b = addr(new R());` now DOES adopt, and frees exactly
once, because the proof shows the result IS the allocation.
