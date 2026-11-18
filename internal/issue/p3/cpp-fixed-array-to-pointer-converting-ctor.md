# A fixed array beside a mismatched array reference is refused instead of using a class-parameter converting constructor

Found 2026-11-14 by the ARRCV2 review (pre-existing on master since ARRCV).

```cpp
// Test/library/cpp_ptrref.hpp
struct ArrayConversion { ArrayConversion(const int*) {} };
inline int array_conversion_pick(ArrayConversion) { return 61; }
inline int array_conversion_pick(const int (&)[2]) { return 60; }
```

```cflat
import cpp "cpp_ptrref.hpp";
extern int main() { int[3] a = {1,2,3}; return cpp_ptrref.array_conversion_pick(a); }
```

clang++ -std=c++20 returns 61 (array-to-pointer decay, then the user conversion `ArrayConversion(const int*)`;
`const int (&)[2]` cannot bind `int[3]`). Master 294d915e0 refuses ("no overload of ... matches"; the
candidate list shows the class parameter as `void**`); before ARRCV (8736c191a) it returned 60, the
extent-mismatched array reference. The fixed-array guard in RankCxxConversionSequences
(cflat/LLVMBackend_Overloads.cpp, "C++ array-to-pointer decay cannot make an array argument scalar") marks
the class parameter non-viable, so ARRCV's all-nonviable array-reference filters refuse the set.
The same conversion from an `int*` lvalue returns 77 on master and clang.

Fix direction: let a fixed-array argument at a class parameter rank as a user-defined conversion through a
non-explicit pointer converting constructor (array-to-pointer + qualification, then ctor), without weakening
pointer-depth checks; narrowing the guard alone did not admit the candidate (Luna round 2: the later rejection
is in constructor viability / SelectCxxConstructor, not isolated). Add an accept leg next to 96022 in
Test/test_cpp_interop.cb once fixed.
