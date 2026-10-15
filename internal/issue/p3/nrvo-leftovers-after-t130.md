# NRVO leftovers after T130 (named return of a struct with C++ fields)

PRE-EXISTING (T130 review 1, 2026-10-12). T130 builds a returned function-scope local in the sret
slot when every return names it. T224 (2026-10-14) landed the single-local cases and T228
(2026-10-15) the several-candidate cases below; what remains is listed under "Still open".

## Landed (T224)

- A local in a nested block, or one that shadows an outer name, that every return names is built
  in the sret slot (scope-resolved candidate; legs Test/test_cpp_interop.cb 13026-13035).
- `construct_at(slot, f(args))` with ONE call returning the C++ class builds the result in the
  slot (0 moves, 0 extra dtors; leg 13038).

## Landed (T228)

- Several candidates (clang's rule): a local is built in the sret slot when every return inside
  its live range (after its declarator, in its scope) names it. Candidates may share the slot
  because their live ranges cannot overlap. Shapes: disjoint blocks, `H a; {H z; if (c) return z;}
  return a;` (z elided, a moved), `H a; if (c) return a; H b; return b;` (b elided, a moved), same
  scope with both returned (neither elided), switch arms, shadowing, loops, throws. Oracle counts
  match clang++ -std=c++20 for every shape; legs Test/test_cpp_interop.cb 13041-13059.

## Still open

1. `construct_at(slot, T(args))` (a constructor expression, not a call) still builds a temporary
   and moves it: clang 1 ctor / 0 dtor, we 2 / 1. Needs the constructor route armed the same way
   (probe: scratch/t224_tmp/probe_cat.cpp row 5).
2. `new S(mk1())` with S a CFlat struct holding a C++ field: "no overload of 'S'" on master and
   after T224 (clang accepts and elides). Copy/move constructor surface for CFlat structs is a
   language decision; not started.

T224 review 1 notes: clang's rule fits all four multi-candidate oracle shapes - a variable is elided when
every return inside its scope, after its declaration, names it (no maintainer decision needed; it is
clang parity). Pre-existing: `C x = (mk1(7));` copies (107 vs clang 7) and `construct_at(slot, (mk1(7)))`
is not armed. (By-value argument temp extra move fixed by T231.)
