# Class-template lookup fails for a C++ header without `#pragma once` (incremental executor)

Rewritten 2026-09-28 by run D1: the original premise (a cast of a scoped enumerator does not fold in a
template-argument position) is disproved - `(int)E.M`, `+ 1`, `* 10` all fold on master 86ccc218. The
filed repro failed for another reason: its header had no include guard.

**Repro** (scratch/repro_keep/d1/r13.h + r13.cb in the main checkout):
```cpp
namespace rvv { template <int N> struct IntBox { int get() const { return N; } }; using IB3 = IntBox<3>; }
```
```cflat
import cpp "r13.h";
extern int main() { rvv.IntBox<2> b = default; return b.get(); }
```
Fails the class-template lookup; add `#pragma once` and it passes; `CFLAT_CPP_INCREMENTAL=0` passes too.
**Cold only.** Reproduce with a fresh cache: `CFLAT_CACHE_DIR=<new dir>` with `<exe dir>/.cflat/runtime`
copied in, then `cflat r13.cb -B -o r13` -> "'rvv::IntBox<2>' does not name a C++ class type in the imported
headers". A second run on that same cache fails again (the failure is not cached). The normal warm cache
(`<exe dir>/.cflat`) passes - possibly only because an earlier `CFLAT_CPP_INCREMENTAL=0` compile stored a
good request result there; a warm pass does not prove the incremental path works.
**Suspect.** The incremental executor includes the header twice into one TU (prefix + request unit), so an
unguarded header redefines the template and the request errors. Real C++ headers are guarded, but plenty of
small user headers are not. **Fix direction.** Make the request units never re-include a header the prefix
already includes (or wrap the user header include in a generated guard); leg with an unguarded fixture.
