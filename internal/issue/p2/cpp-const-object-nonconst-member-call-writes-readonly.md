# P2: Remaining const C++ receiver gaps

The member-overload tracking fixes in B20, B20c, and B20d cover local const objects,
const namespace objects, const references, and const pointees for the resolved shapes below.
These are the remaining independently reproduced gaps.

## 2. Pointee const through non-local CFlat storage

Const pointee metadata is lost after a pointer is stored in a CFlat field, a CFlat global, or
a list element. Examples: `s.p = rv.pcp(); s.p->only();`, `g = rv.pcp(); g->only();`, and
`l[0]->get()`. Calls to a non-const-only member can then be accepted on a const pointee.

## 4. Loop back-edge const-pointer dataflow

The receiver analysis sees the call before a const-pointer assignment on a loop back-edge:
`rv.P* q = rv.pmm(); for (2x) { q->only(); q = rv.pcp(); }`. Clang rejects the assignment
from `const P*` into `P*`; CFlat currently misses it on this path.

## 6. Virtual-base const/non-const twin resolution

Member lookup does not find the const/non-const overload twin through a virtual base path.
The existing non-virtual base coverage does not exercise this lookup.

## Review leftovers

- Array elements of a const parent's non-const array field remain mutable (`ch.arr[0].only()`
  is accepted and `ch.arr[1].get()` selects 13; clang rejects and selects 3 respectively).
- Taking the address of a const field does not carry pointee const (`auto q = &h.c; q->get()`
  selects 13; clang selects 3).
- Calling a static member template through an object (`ch.ts(2)`, `rv.gcH.ts(2)`) fails to
  bind, while the class-qualified call succeeds.

Current related coverage is in `Test/test_cpp_interop.cb` with C++ fixtures in
`Test/library/cpp_interop_basic.h` and `scratch/repro_keep/b20d/rv1/rv.h` (main checkout).

Also open (b20d review 2, same on master; probes scratch/repro_keep/b20d/rv2/):
- A `const auto` copy of a C++ record is not a const receiver: `get()` picks the non-const overload (13) and `only()` is accepted; clang picks const (3) and refuses.
- A pointer declared to const, `const rv.C* pm; pm->get()`, picks 13; clang 3.
