# `unique` assignment laundering: residue after the return-identity alias proof landed

The 2026-08-10 RULING's item 1 (the return-identity alias proof) and the FIELD-OVERWRITE half of
item 2 have LANDED. What is left of this file is the one shape from item 2 that a PROOF cannot
reach, plus the two accepted caveats. Item 3 (the `llvm.mem*` DESTINATION analog) stays deferred
and unreachable, exactly as ruled.

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

## Residue - a projected pointer handed to an UNANALYZABLE callee (LEAKS, sound)

```cflat
void show() { printf("[%s]\n", tag.data()); }   // on an owning receiver temp
```

`data()` is now transparent (its parameter escapes only via its return, so the walk keeps following
the buffer pointer in `show`'s frame). The pointer then reaches `printf`, a VARARG callee whose
variadic slots are read back through a `va_list` the walk cannot follow. Answering "does not retain
past my call" there is not a proof - it is a decision to TRUST an unanalyzable callee, and the
harm it buys is a use-after-free (the temp is freed at the statement boundary while a stashed copy
of the buffer pointer survives), not a leak. That inverts this family's polarity - free only what
you can prove is unreferenced - so it was deliberately not taken. Closing it needs either a
va_list-aware walk of the variadic callee's body, or an explicit ratified decision that an
external/variadic declaration never retains, with `free`-family deallocations excluded so
"the callee freed the object" keeps answering "retains".

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
