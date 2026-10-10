# Static local run-once guard is set before the initializer runs; a throwing C++ initializer is never retried

Found by T62 review 2 (pre-existing on master).

## Repro shape
```cflat
import cpp "maythrow.hpp";   // r::make(int) throws on the first call only
int f(int a) { static auto g = r.make(a); return g.v; }   // explicit `static r.T g = ...` same
// call 1 throws (caught in C++ or terminates per test harness); call 2 reads g
```
cflat: the guard flag is stored before the initializer, so call 2 skips init and reads a zeroed,
never-constructed object. clang++ -std=c++20: [stmt.dcl]/4 - if initialization exits by an
exception, it is not complete and is retried on the next entry.

## Fix direction
Set the guard after the initializer completes (or clear it on the unwind path / use
__cxa_guard_acquire/release/abort semantics). Needs exception paths through C++ calls in CFlat;
check how T62's ternary EH path destroys the global.
Probes: scratch/repro_keep/t62_rev/r2/.
