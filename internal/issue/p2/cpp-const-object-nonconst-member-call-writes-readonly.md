# Const C++ receiver leftovers after B20

Summary: B20 landed in 44d3321c (2026-09-29). Member calls, member binary operators and member unary operators on a const namespace global, a const& result, or a const T* reached through -> now pick the const overload like clang. A named call with no const overload is refused. Probes are in scratch/repro_keep/b20rv2/ and scratch/repro_keep/d7r1/. Still open:
1. Const member FIELDS are not tracked. The clang record carries no field-const flag. Adding one needs a flag in the extraction, a header-cache version bump, and a marker on field access.
   - Example: `-h.c` / `!h.c` on `struct HC { const CM2 c{}; }` gives 13 / 60, and mutates the field; clang gives 3 / 50.
   - `h.c.get()` gives 13.
2. A local `const T*` loses its pointee constness: `rv.A2* q = rv.a2p(); q->add(1);` SIGBUSes on master and on the branch. clang refuses the declaration.
3. A non-const-only member called through a const T* result or a type-const receiver still runs; clang refuses. The refusal covers only const globals and const& results.
4. An inherited non-const-only member on a const global crashes instead of being refused.
5. A virtual base does not resolve the twin.
6. A mutable object whose two bases both define the pair is not flagged as ambiguous.
7. A mixed set such as `get()` const/non-const plus a non-const-only `get(int)`, called as `get(5)` on a const object, gives a generic no-match error instead of clang's const-specific one.
