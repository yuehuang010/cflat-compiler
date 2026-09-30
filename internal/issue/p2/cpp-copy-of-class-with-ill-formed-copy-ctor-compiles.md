# Copying a C++ class whose implicit copy constructor fails to instantiate compiles

Found by the R1 review (perf timebox 2026-09-29). This bug is already on master (93b361f6); R1 did not introduce it.

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

## Update 2026-09-30 (D8 review): now a crash, not a silent compile
On master 93b361f6 the same shape crashes the compiler with exit 139 (null deref in clang
CodeGen ScalarExprEmitter::VisitExpr). This happens whether the holder's copy constructor is
implicit or `= default`. Repro kept in scratch/repro_keep/d8/ (d8u.h + u_copy.cb, d8c.h + c_copy.cb):
```cpp
template <class T> struct Bad { static_assert(sizeof(T) == 3, "d8u bad"); static const int value = 1; };
template <class T> struct M { int v = 2; M() {} M(const M& o) : v(o.v + Bad<T>::value) {} };
struct H2 { M<int> m; int x = 4; H2() = default; H2(const H2&) = default; };
```
```
d8u.H2 h = default;
d8u.H2 h2 = h;   // exit 139; expected: use-site error relaying clang's static_assert text
```
Raise the priority: this is a crash, not just a missing diagnostic.
