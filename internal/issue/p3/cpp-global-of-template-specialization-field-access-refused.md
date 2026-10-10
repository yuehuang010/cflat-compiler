# Field read on a C++ global whose type is a template specialization is refused

Pre-existing (master same); found by the T66 review 2 (2026-10-07).

```cpp
namespace r67 { template <class A, class B> struct P { A t; B u; }; P<int, int> p11{1, 2}; }
```
```cflat
int v = r67.p11.u;   // cflat: "'u' is not a member of namespace"; clang++: 2
```
The member path appears to treat `p11` as a namespace segment instead of a variable when its type
is a class template specialization (plain struct globals work). Probe:
scratch/repro_keep/t66_rev/rev_t66_fp2/fp2.h (+ fp2_oracle.cpp).

Fix direction: resolve `ns.var.field` by variable lookup first; check whether template-
specialization globals are bound (harvested) at all - compare `r67.p11` alone, `&r67.p11`.
