# RULING OWED: C++ type reached through a CFlat-declared sub-namespace of a C++ namespace

Raised by T61 review 2 (behaviour change landed with T61, non-blocking).

## Shape (scratch/repro_keep/t61_rev/rev2_t61/c_cx.cb)
```cpp
namespace w { struct P { int v; }; }          // imported C++ header
```
```cflat
namespace w.ext { int helper() { return 1; } } // CFlat extends the C++ namespace name
w.ext.P p = default;                           // P lives in C++ w, not in w.ext
```
Before T61: resolved by walking outward to `w.P` (ran, 13). After T61: "cannot find the type
'w.ext.P'" - under a C++-rooted prefix only the exact member counts for C++ entities, matching C++
qualified lookup (clang rejects `w::ext::P` when ext does not contain P). CFlat entities under the
same prefix still walk outward as before. No test uses the old spelling.

## Question for the maintainer
Keep the refusal (C++ lookup rules for C++ entities everywhere), or let a CFlat-declared namespace
segment re-enable the outward walk for C++ entities too?
