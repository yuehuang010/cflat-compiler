# `(void)expr` on a NON-CONSTANT emits `bitcast <T> to void` and fails module verification

Filed 2026-08-09 while fixing
`return-value-void-mismatch-fails-module-verification`. **Pre-existing and unrelated to
`return`** - it fires on a plain expression STATEMENT too. Measured Release, macOS arm64, warm
`--init-local`, on the merge base `680696c` (`scratch/prefix_bin/cflat`) and on the fix commit:
identical on both.

Severity: **P3**. Locationless verifier dump, which is normally the P1 rule, but `(void)x` as a
discard is a C idiom cflat has no need for - an expression statement already discards - so the
reachable-from-ordinary-source half of that rule is weak here.

## Repro

Nothing to do with `return` - a bare statement is enough
(`scratch/rv_cv11_castvoid_statement.cb`):

```cflat
void f(int x) { (void)x; printf("cv11\n"); }
extern int main() { f(3); return 0; }
```

```
Module verification failed:
Invalid bitcast
  bitcast i32 %0 to void
Error: module verification failed.
```

The CONSTANT spelling is fine - `(void)0` as a statement compiles and runs
(`scratch/rv_cv12_castvoid_const_statement.cb`, prints `cv12` on both binaries), because a
constant folds to LLVM's token `none` instead of going through the bitcast.

Same defect reached through a return, both directions
(`scratch/rv_cv2_void_castvar_in_void.cb`, `scratch/rv_cv3_void_castintcall_in_void.cb`):

```cflat
void f(int x) { return (void)x; }              // Invalid bitcast, both binaries
void g() { return (void)someIntCall(); }       // Invalid bitcast, both binaries
```

On `680696c` these printed the invalid-bitcast line AND a return-instruction line; the fix
commit removes the return half, so the bitcast is now the whole dump. Not a regression - rc 1
with no binary and no source location before and after - but it is why
`void f(int x) { return (void)x; }` is NOT in that commit's accept set while
`void f() { return (void)0; }` is.

## The `(void)<void call>` leg, and what `fix/voidcall` changed about it

Added 2026-08-09 during the review of `fix/voidcall`. `(void)g()` on a `Lambda<void()>` is
rejected on BOTH binaries, so it is not a regression, but the message changed
(`scratch/rev_p08_voidcast_stmt.cb`):

| binary | `(void)g();` as a statement |
|---|---|
| base `75b4275` | `'g()' does not name a value, so it cannot be cast to 'void' ...` |
| `fix/voidcall` | `call through function value 'g' returns 'void', so it produces no value to consume - call it as a statement` |

The new message arrives because the cast parses its operand at `ResultUse::Value`. That is a
CONSTRAINT on the fix below, not a separate defect: the `void` destination arm must parse its
operand in a DISCARD position, or the void-closure gate rejects it before the arm is reached and
`(void)g()` stays unusable even after this issue is closed.

## Root cause

`MainListener::ParseCastExpression` (`MainListener_Expressions.cpp:4963`) has no arm for a
`void` destination type. A non-constant operand falls through to the generic reinterpret and
emits `CreateBitCast(value, voidTy)`, which is never valid IR. The constant path escapes only
because it folds before reaching the bitcast.

## Fix direction

Give the cast an explicit `void` destination arm: evaluate the operand for its side effects,
then produce a NamedVariable with `TypeAndValue.TypeName == "void"` and no `Primary` - the same
shape a void CALL already hands back, which the return lowering and the expression-statement
discard both already understand. Emitting nothing at all is what C means by `(void)expr`.

Guard the arm on the destination being `void` and NOT a pointer, so `(void*)p` keeps
reinterpreting (`scratch/rv_cv10_voidptr_cast_stays_value.cb` compiles and prints `cv10=3` on
both binaries and must keep doing so).
