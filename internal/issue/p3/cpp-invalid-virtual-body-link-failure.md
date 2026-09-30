# Invalid virtual member body of a C++ class template surfaces as a link failure

Found by the R4 review round 1 (perf timebox 2026-09-29, member bodies on call). Same on master.

## Symptom
```cpp
namespace rv { template<class T> struct V { virtual ~V() {} virtual int f() { return T::nope; } int g() { return 1; } }; }
```
`rv.V<int> v = default; return v.g();` fails at link: "Undefined symbols: rv::V<int>::f() referenced
from vtable". clang++ rejects the program at compile time (the vtable ODR-uses every virtual, so
`f`'s body is instantiated and fails). Valid virtuals work. Repro: scratch/repro_keep/r4rv/p3/a.cb.

## Fix direction
When a record's vtable is emitted (construction of a polymorphic C++ type), demand-check every virtual
member body of that record and relay a failure as one LogError at the construction site ("clang: "
prefix), like the member-call demand check. Never a link failure.
