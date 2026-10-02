# A C++ class temporary passed by value to a C++ function is moved twice

## Repro

```cpp
// counts.hpp
#include <cstdio>
namespace r3 {
inline int c = 0, cp = 0, mv = 0, d = 0;
inline void report() { std::printf("counts=%d,%d,%d,%d\n", c, cp, mv, d); }
template<class T, class F = void (*)(int*)> struct Count {
    int value;
    Count(int v) : value(v) { ++c; }
    Count(const Count& x) : value(x.value) { ++cp; }
    Count(Count&& x) : value(x.value) { ++mv; }
    ~Count() { ++d; }
};
template<class T, class F> int take(Count<T, F> x) { return x.value; }
}
```

```cflat
import cpp "counts.hpp";
extern int main() { r3.take<int, int>(r3.Count<int, int>(7)); r3.report(); return 0; }
```

## Observed vs expected

Observed (master and S7 branch, flat or nested template arguments alike): `counts=1,0,2,3`. The
prvalue is built in a temporary and moved twice on its way into the by-value parameter.
Expected (clang++ -std=c++20, guaranteed elision): `counts=1,0,0,1`.

A class with no usable move/copy (an owner with only `Owner(T*, F)` and a destructor) is refused
with "the generated wrapper could not be registered", while clang constructs it directly into the
parameter and destroys it once.

## Suspected area

The by-value C++ class argument path of the generated call wrapper (thunk takes the class by value
and the caller materializes, then moves, the argument). The prvalue should be constructed straight
into the parameter slot, as the `T x = T(args)` local declaration now does
(`TryDeclareForeignCxxLocal` / `ForeignCxxConstructArgs`). Found in S7 round 3 review (P2-1).

## Progress (N67, partial)

Root cause: the generated function-template wrapper (`RequestCxxFunctionTemplate`, class-argument
loop near `classRvalue`) spelled a class rvalue argument BY VALUE (`T p0`), so the caller moved the
temporary into the wrapper parameter and the wrapper moved it again into the callee's parameter
(`take(static_cast<T&&>(p0))`). Landed: a class rvalue now crosses as `T&&` (one move, no copy):
prvalue 1,0,1,2 (was 1,0,2,3), `move x` 1,0,1,x (was 2 moves, matches clang), named lvalue
unchanged (1 copy). Still open: true guaranteed elision (clang 1,0,0,1) and the no-copy/no-move
Owner prvalue (still refused with deleted-constructor). Both need the wrapper to receive the
prvalue's constructor ARGUMENTS and write `take(T(p0, ...))` itself; the caller currently emits the
temporary's constructor call before the wrapper is requested, so the construction would have to be
deferred (or re-derived from the argument expression, as `ForeignCxxConstructArgs` does for locals).

Also (N67 review): a prvalue of a class with a DELETED move ctor (`r2.take(r2.DelMove(4))`) is refused
on master and branch; clang accepts via guaranteed elision. Same root as above (wrapper must build the
temporary itself).
