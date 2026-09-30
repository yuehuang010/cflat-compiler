# P3: C++ ctor in expression position - remaining `const T&` copies and enum `T&&` refusal

The exact-type scalar LVALUE case (local, alias/by-value param, nested CFlat field, `->` field,
pointer index, C++ field, global) is fixed in `fix/ctor-expr-const-ref-lvalue`: `C(L, &L)` with
`C(const long& x, long* p) { *p = 9; v = x; }` now reads 9 like clang. Remaining (B4 review 1,
probes in the branch worktree scratch/r1/ at landing time):

## 1. Reference-returning call results and xvalues still copy (expression position only)
```cpp
namespace rs { struct CL { long v; CL(const long& x, long* p) { *p = 9; v = x; } };
inline const long& cref(const long& x) { return x; } inline long& refl(long& x) { return x; } }
```
```cflat
long L = 5; long a = rs.CL(rs.cref(L), &L).v;   // clang 9, cflat 5
L = 5;      long b = rs.CL(rs.refl(L), &L).v;   // clang 9, cflat 5
L = 5;      long c = rs.CL(move L, &L).v;       // clang 9, cflat 5 (xvalue binds const T&)
```
The DECLARATION form `rs.CL x = rs.CL(rs.cref(L), &L);` already reads 9: extend the
expression-position address path (LLVMBackend_CInterop.cpp ~19638, MainListener_PostfixExpression.cpp
~7079) to reference results and `move` by mirroring what the declaration path does.

## 2. Ternary lvalue argument copies in BOTH forms
`rs.CL(c ? L : L2, &L)`: clang binds the selected arm (9), cflat copies (5) in declaration and
expression position. The ternary-as-lvalue ruling (2026-09-20: ternary collapses to the selected
arm) says it should bind.

## 3. Enum enumerator into a `T&&` parameter is refused
Fixture `cpp_interop_ctorref.h`: `ExprEnumPick(ExprUnscoped&&)` and `ExprEnumPick(const long&)`;
`ExprEnumPick(ExprEnumValue)` is rejected with `parameter 'x' is an rvalue reference; pass 'move
<argument>' or a temporary value`. clang selects `ExprUnscoped&&` (discriminator 1). A scoped enum
and an integer literal into `T&&` are adjacent cases to verify. Direction: the enumerator's
prvalue category or enum identity is lost between the C++ enum registry, postfix argument
construction and `SelectCxxConstructor`'s rvalue proof - trace it through the call, do not infer
category from the numeric LLVM constant alone.
