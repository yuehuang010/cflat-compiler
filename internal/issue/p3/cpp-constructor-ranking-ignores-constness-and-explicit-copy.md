# C++ constructor ranking ignores constness and explicit copy restrictions

Summary: By-value copy can select an explicit copy constructor, and one-argument constructor calls ignore argument constness.
Repro: `explicit EC(const EC&)` plus `EC(EC&)` with const source; `S1(A&)` called with const A; overload sets `S4` and `S5` from T461 r2.
Master vs expected: master accepts cases clang refuses and returns 107 vs 207, 107 vs 57, or 207 vs 77 in the described overloads.
Found: T461 r2 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX11): NOT FIXED, both halves. (a) FindCxxCopyCtor ignores isExplicit, so copy-init of a
const source takes the explicit `const EC&` copy ctor. The fix (skip explicit ctors in copy-init) makes
the existing leg cppexp ExCopy (Test/test_cpp_interop.cb near line 13056, Test/library/cpp_interop_explicit.h)
fail: that leg pins `ExCopy b = a;` with an explicit-only copy ctor as working (+1000), which clang
refuses. Needs a maintainer ruling on that leg (read-only here), so the change was reverted.
(b) SelectCxxConstructorListed lets a non-const class-reference ctor bind a const object (`S1 s = ca;`
with `S1(A&)`). A constness check that drops the candidate changed the clang text of the existing leg
cpi_t42 (expected "no viable conversion from 'const CvSource' to 'CvTarget'"), so it was reverted.
Fix direction for (b): when the only rejected candidate is a non-const class-ref ctor and the argument
is a const object, ask clang copy-init and relay its refusal through ConvertThroughCxxConvertingCtor.
(c) The S4/S5 overload sets from T461 r2 have no repro in the repo and were not reproduced.
