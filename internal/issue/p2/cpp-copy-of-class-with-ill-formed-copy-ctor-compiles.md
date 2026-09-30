# Copying a C++ class whose implicit copy constructor fails to instantiate compiles

Found by the R1 review (perf timebox 2026-09-29). This bug is already on master (f7c5b7d0); R1 did not introduce it.

## Repro
```cpp
// rvpc.h
namespace rvp {
template<class T> struct Bad { int v = 1; Bad() {} Bad(const Bad& o) : v(o.v) { static_assert(sizeof(T) == 0, "Bad copy"); } };
struct BadHolder { Bad<int> b; int x = 1; };
inline int bad_x(const BadHolder& h) { return h.x + h.b.v; }
inline BadHolder* new_bad() { return new BadHolder(); }
}
```
```cflat
import cpp "rvpc.h";
extern int main()
{
    rvp.BadHolder* h = rvp.new_bad();
    rvp.BadHolder c = *h;          // copy: BadHolder's implicit copy ctor calls Bad<int>(const Bad&)
    printf("%d\n", rvp.bad_x(c));
    return 0;
}
```
clang++ -std=c++20 rejects the same program at the static_assert ("Bad copy"). cflat compiles it and runs it (master and R1 alike).

## Expected
The copy is refused at the CFlat copy site, with clang's text relayed ("clang: " prefix), like every other C++ body that fails to instantiate.

## Direction
Find how the copy is lowered for a record whose implicit copy constructor is non-trivial. It is either a memberwise/bitwise copy that never calls the C++ copy ctor, or a call to a definition that was never instantiated. Whichever it is, the implicit ctor's instantiation failure never reaches the use site. Probe files kept in scratch/repro_keep/ (p3b.cb, p3b_oracle.cpp, rvpc.h).
