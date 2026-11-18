# CFlat `move` of a C++ class: sink shapes still byte-copy (N56 leftovers)

Ruling 2026-10-01: CFlat `move x` of a C++ class value mimics std::move. N48 + N56 cover init, auto, assignment (non-trivial), return, by-value arguments. The 2026-11-07 sinkshape pass resolves the simple fixed-array initializer and deleted-move/explicit-move fixed-array cases; remaining probes still reproduce against clang++ -std=c++20:
1. Ternary arms: `auto c = cond ? move a : move b` and arms reading an element still byte-copy (or leave a zeroed source) for a trivially copyable class with a constructor template; a deleted move at a ternary sink is still accepted.
2. `[cpp] struct Box { edge.Triv item; }; Box b = move a` (class-level move of a CFlat `[cpp]` struct whose field needs a selected constructor) still byte-copies the field: value 7, clang 107/m1. See the 2026-11-10 section below.

Repro for 1 (item 3, the brace-element prvalue, landed with BRACEPRV; header Test/library-shaped, namespace e6mv, trivially copyable with defaulted copy ctor and forwarding ctor template):

```cpp
struct Tc { int v; Tc() : v(0) {} Tc(int x) : v(x) {} Tc(const Tc&) = default;
            template <class U> Tc(U&& u) : v(u.v + 100) { ++moves; } };   // moves: global counter
struct HolderC { Tc t; };
```
- 1 (CFlat): `int k = 1; Tc a = Tc(7); Tc b = Tc(8); auto c = k ? move a : move b; printf("c=%d moves=%d", c.v, moves);`
  master prints `c=7 moves=0`; clang++ prints `c=107 moves=1` (k=0: clang `c=108 moves=1`, master `c=8 moves=0`).
- 3 (CFlat): `HolderC b = {Tc(7)}; printf("value=%d moves=%d", b.t.v, moves);`
  master prints `value=107 moves=1`; clang++ prints `value=7 moves=0`.

- 1b (deleted move at a ternary sink): `struct NoMove { int v; NoMove(int); NoMove(const NoMove&) = default; NoMove(NoMove&&) = delete; };` `auto c = k ? move a : move b;` clang++ refuses (call to deleted constructor); master accepts and prints `c=7`.
Still open: 1 and 1b (ternary arms; arms reading an element not yet probed), 2 (needs a ruling, below), 3 (brace element deferral).

## 2026-11-10: attempt on item 2 dropped (E6SINK)

Widening `NeedsCxxLoweringType` so a `[cpp]` struct with a trivially copyable member that has a constructor template gets a lowered move fixes the
single-field case (107/1, clang) but was NOT landed, so item 2 is STILL OPEN on master. Review found two regressions from master, values below are value/moves:
- Plain struct loses NRVO: `struct Box { edge.Tc item; }; Box mk() { Box l; l.item.v = 7; return l; }` master and clang 7/0, fix 107/1. The widened
  predicate makes the return sret, but the NRVO gate requires `HasForeignNontrivialCxxField` (`MainListener_Declarations.cpp` ~6099-6108).
- Multi-field shapes: `[cpp] struct Box { int tag = 5; Tc item; }`, Tc then int: master and clang 7/0, fix 107/1; two Tc fields master and clang 1007/0, fix 101107/2. Clang's
  implicit move merges a run of two or more fields into one memcpy and uses the selected constructor only for a lone field; the standard reads otherwise.
Needs a maintainer decision before retrying: ask clang for the whole record's move for `[cpp]` structs (plain structs would still need a rule) or mirror clang's run rule in `EmitLoweredMemberwiseMove`.
Fixture: `struct Tc { int v = 0; Tc() = default; template <class U> Tc(U&& o) : v(o.v + 100) { ++moves; } };` trivially copyable, constructor template chosen for a non-const lvalue.
