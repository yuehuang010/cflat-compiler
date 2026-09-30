# C++ multi-word conversion operators (`operator unsigned`, `long long`, `signed char`, `long double`) silently mis-picked (p2)

## Summary
Implicit conversion from a C++ class to an unsigned destination picks the wrong conversion
operator with no diagnostic, where clang either picks the exact unsigned operator or reports an
ambiguity. Same on master and after B15 (not a regression). Silent wrong-operator = wrong value.

## Repro (clang++ -std=c++20 as oracle)
```cpp
// rk.h
namespace rk {
inline int last = 0;
struct BU { operator bool() const { last = 1; return true; } operator unsigned() const { last = 5; return 4u; } int pad = 0; };
struct LU { operator long() const { last = 2; return 9; } operator unsigned() const { last = 5; return 4u; } int pad = 0; };
}
```
```cflat
import cpp "rk.h";
extern int main() { rk.BU p = default; rk.last = 0; u32 v = p; printf("%d %d\n", rk.last, (int)v); return 0; }
```
clang: `5 4` (operator unsigned, exact). cflat: `1 1` (operator bool). `LU` -> cflat picks
operator long. `operator unsigned char` / `operator unsigned short` with `u8` / `u16`
destinations mis-pick the same way (review cells c1, c10, c12 of B15 round 2).
The explicit cast `(u32)p` binds operator unsigned correctly.

Same shape for `i64` / `u64` with `operator long long` / `operator unsigned long long`, `i8` with
`operator signed char`, `longdouble` with `operator long double`: cflat picks `operator bool`,
clang the exact operator (B15 review 3 probes).

## Root cause (confirmed, B15 review 3)
cflat/CClangExtract.cpp (~2226) registers each conversion operator under clang's own spelling
(`operator unsigned int`, `operator long long`, `operator signed char`, `operator long double`).
`CxxConversionOperatorTo` (cflat/LLVMBackend_CInterop.cpp) looks candidates up by CFlat
spellings (`candidateNames` over `ScalarConversionSpellings()`), so every multi-word C++ spelling
is invisible to both the exact lookup and the standard-conversion fallback, and another operator
wins silently. The explicit-cast path maps the spelling differently and binds correctly.

## Fix direction
Map CFlat scalar spellings to the C++ operator spellings clang records (unsigned int/char/short/
long/long long, long long, signed char, long double) in candidateNames - ideally through
`CanonicalPrimitiveTypeName` (cflat/LLVMBackend.h), which should also replace the local
`canonical` lambda in CxxConversionOperatorTo - share it with the explicit-cast path, and add legs
for u32/u8/u16/i64/u64/i8/longdouble destinations (clang oracle).
