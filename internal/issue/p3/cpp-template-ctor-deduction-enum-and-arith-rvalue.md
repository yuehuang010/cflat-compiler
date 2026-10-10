# P3: C++ template deduction - narrow-int promotion and `long` ternary still deduce the wrong T

Remainder of the B4 review 1 finding after T58 (2026-10-06). T58 fixed enum lvalues/enumerators,
`long`/`unsigned long`/`long long` arithmetic (usual arithmetic conversions, parenthesised operands,
unscoped enum operands promoted) for ctor, by-value, `T&&`, `const T&` and member templates, and
the same-width wrapper ambiguity (`byval(ulong + 1)` after `byval(long)`). Three cells remain
(matrix scratch/m/ in the T58 worktree; oracle clang++ -std=c++20):

```cpp
namespace t { template <class T> long byval(T); }   // reports which T was deduced
```
```cflat
char ch = 3; short sh = 3; long l = 3; long l2 = 5; bool c = true;
t.byval(ch + ch);       // clang T = int, cflat a narrow 8-bit T (CFlat types the result i8)
t.byval(sh * sh);       // clang T = int, cflat a narrow 16-bit T (i16)
t.byval(c ? l : l2);    // clang T = long, cflat T = long long
```
Promotion: CFlat keeps narrow arithmetic results narrow (native typing, not to change); the C++
identity `int` needs the call to widen the value to i32 before the wrapper, so it is not just a
spelling change (CxxScalarArithmeticIdentity in MainListener_Expressions.cpp answers `int` but
refuses it because the value is i8/i16).
Ternary: owned by T55 (ParseTernaryBranches / ParseAssignmentExpressionNamed); re-check the cell
(T55 landed e2970f82). T58 round 5 (b0bce9cb) now carries CxxArithIdentity through `?:` arms - re-measure.

## After T58 review 1 (2026-10-06)

- Shift and bitwise results carry no C++ arithmetic identity: `byval(l << 1)` deduces long long (clang long),
  `byval(l & ul)` long long (clang unsigned long). Only + - * / % set it (master same).
- Pre-existing, master same: `erv(E&&)` beside `erv(const E&)` given an enumerator picks `const E&` (clang `E&&`);
  `&sv.e` accepted (clang refuses); `auto a = l + 1;` then deduces long long (clang long);
  `byval(l + 1.0)` deduces the wrong type (clang double). Probes: scratch/repro_keep/t58_rev/.

## After T58 review 2 (2026-10-06)

- (T58 review 2, master same) `byval(sizeof(long) + 1)` deduces long long (clang unsigned long); a free C++
  operator with a long / unsigned long parameter refuses a CFlat long operand (`l - tx` with operator-(long, X):
  "no overload", arg shown as long long) and `tx * (i + 1)` is accepted where clang is ambiguous; a deduction
  conflict in std.max is reported as "'max' is not a member of namespace 'std'".

## After T58 review 4 (2026-10-06, pre-existing, master same)

- Pointer difference deduces `long long` (`unsigned long long` for `ulong*`) where clang deduces
  `ptrdiff_t` (`long`).
- `vv.W w(1e3 + 1);` (C++ class direct-init with an arithmetic argument) is refused "cannot
  understand" on master too - may be by design (CFlat spells construction `vv.W(...)`); confirm.
- Probes: scratch/repro_keep/t58_rev/rev4/.
