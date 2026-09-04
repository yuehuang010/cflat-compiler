# `unique<T, 64> p = new T();` is rejected while the `alignas(0, 64) unique T*` sugar accepts it

Filed 2026-09-03 from the opus review of the aligned-field desugar (q01 member 4). Surface
inconsistency, not a safety bug: the rejected form was a mis-aligned block freed through
`__delete_aligned` on the builtin path, so the error is strictly safer than before.

## Repro

```cflat
struct R { int v = 0; };
int main()
{
    unique<R, 64> p = new R();              // error, see below
    alignas(0, 64) unique R* q = new R();   // accepted: same type, sugar spelling
    return 0;
}
```

Diagnostic: `alignment mismatch when moving into the 'move' parameter of 'unique<R, 64>': the
parameter must align at 64, but the argument was allocated at 0`. Same for a struct field
written with the explicit generic. Workaround: `alignas(0, 64) R* t = new R(); unique<R, 64> p =
move t;`.

## Root cause

Only the sugar path seeds `pendingInitAllocAlign` from the declarator's `AllocAlignValue`
(MainListener_Expressions.cpp ~9839 and the field-default / brace-init inheritance added in the
desugar commit). The wrapper type's `ALIGN` template argument is never consulted, so a direct
`new` on the right-hand side of an explicitly spelled `unique<T, N>` allocates at ordinary
alignment and the move-parameter alignment check rejects it. The message also names a
synthesized parameter the user never wrote.

## Fix direction

When the declared type is core `unique<T, N>` with N > kDefaultNewAlign, seed the inbound
allocation alignment from N exactly as the sugar does (both declaration and assignment doors, both
ParseDeclarationSpecifiers copies if the fold happens there). Reword the mismatch message for the
`unique<T, N>` receiver to name the declared type, not the `move` parameter of the constructor.
Legs: accept leg in Test/test_core.cb (explicit spelling, 1 destructor, IR `___delete_aligned`),
`expect_error` for a genuine mismatch (`unique<R, 64> p = new R() alignas(0, 32);`) in
Test/errors/err_align_alloc_mismatch.cb.

Related (pre-existing, noted in the same review): `unique<R, 64> = move unique<R, 128>` is
correctly rejected but with the unrelated message "cannot cast an aggregate value - a fixed array
decays to a pointer to its first element".
