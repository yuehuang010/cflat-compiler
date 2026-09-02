# Explicit `delete p` on a `move T*` parameter ignores the hidden raw-array count

Found 2026-09-02 while measuring internal/issue/p2/unique-field-heap-array-through-move-param.md.
Memory-safety class: n-1 element destructors skipped.

## Repro

```cflat
int gd = 0;
struct Y { int v = 0; ~Y() { gd = gd + 1; } };
void sink(move Y* p) { delete p; }
int main() { sink(new Y[3]); return gd; }   // observed 1, expected 3
```

Probe: scratch/rt/p1.cb on the unique<T> branch. Replace the body with `{ }` (scope-exit
cleanup) or `{ inner(move p); }` (forwarding) and gd is 3: those paths branch on the count.

## Root cause

A `move T*` parameter arrives with an `i64 <name>.raw_array_count` argument
(`ParameterCarriesRawArrayCount`), stored in a per-parameter slot with `AllocatedByRawNewArray`
set (LLVMBackend_CodegenHelpers.cpp ~186-210). Scope-exit cleanup consults it
(LLVMBackend_OwnershipTemps.cpp ~1239: `raw_array_dtor_array` / `raw_array_dtor_scalar`
branch), but `ParseDeleteExpression` (MainListener_Expressions.cpp ~11556) emits a single
destructor call plus `operator delete` and then stores -1 into the slot. `--symbol-dump-ir
function:sink` shows the scalar `del_dtor` path followed by the unused counted cleanup.

## Fix direction

In the plain `delete x` arm, when the operand NamedVariable has `RawArrayLengthStorage` or
`RawArrayLength`, emit the same count branch scope-exit cleanup uses (factor the branch out of
the cleanup path rather than copying it). `delete[n]` / `delete[_]` / `delete[]` keep their
documented meanings. Positive leg: extend the `runRawCount*` family in Test/test_core.cb with a
`move T*` sink that deletes explicitly.
