# Reading a C++ static inline member whose initializer fails to instantiate prints 0

Found by the D8 review (perf timebox 2026-09-29d). The bug is already on master (93b361f6);
D8 did not introduce it.

## Repro
```cpp
// d8t.h
namespace d8t {
template <class T> struct Bad { static_assert(sizeof(T) == 3, "d8t bad"); static const int value = 1; };
template <class T> struct TS { static inline int bad = Bad<T>::value; static inline int good = 8; };
}
```
```
import cpp "d8t.h";
extern int main() { printf("%d\n", d8t.TS<int>.bad); return 0; }
```
This compiles with no diagnostic and prints 0. clang++ rejects the equivalent C++ with the
static_assert. Repro files: scratch/repro_keep/d8/d8t.h, t_sbad.cb.

## Expected
A use-site error at the member read that relays clang's text with the "clang: " prefix, the
same way a failed member body is reported under ruling R4.

## Fix direction
The static data member's initializer is instantiated on demand, but its failure is never mapped
back to the read. The variable is probably emitted zero-initialized or left as an external
declaration. Route variable-template and static-member initializer failures through the same
demand verdict path that function bodies use.
