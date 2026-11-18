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

## Landed (T240)

- `construct_at(slot, T(args))`, `construct_at(slot, (f(args)))` and `construct_at(slot, (T(args)))`
  build the C++ class in the slot (clang 1 ctor / 0 dtor); a template converting constructor keeps
  its value (30, not a moved 31). Legs Test/test_cpp_interop.cb 13060-13065.

## Still open

1. BRACEPRV order rule: a constructor element is deferred only when every LATER element is a literal or a
   construction from literals, and no earlier element left a CFlat class temp. So `int k = 4; Outer o = { BS(k, 1), k };`
   (braceprv.hpp shapes) still moves: 1 ctor / 1 move / 1 dtor in scope, clang 1/0/0. Lifting it needs a proof that the
   later element cannot observe the constructor (a local read the constructor may write through a pointer).
2. Paren-aggregate `CM l = CM(S(5), 3);` with `struct CM { S c; int k = 9; };` (see cpp-paren-aggregate-prvalue-member-move.md).
3. BRACECALL call-element gaps, measured 2026-11-17 (value; ctor/copy/move/dtor; order):
   - `{ mk(1), side() }`: master and fix are 5; 1/0/1/2; order 19. Clang is 5; 1/0/0/1; order 19.
     Test leg `Test/test_cpp_interop.cb` 13611 pins this known ordering gap, not clang parity.
   - A call whose argument is itself a call or temporary, `{ mk(g()), 1 }` and `{ mk(S(2).v), 1 }`,
     stays on the eager path. `mk(g())`: master and fix are 3; 1/0/1/2; order 73, clang is 3; 1/0/0/1; order 73.
     `mk(S(2).v)`: master and fix are 2; 2/0/1/3; order 22, clang is 2; 2/0/0/2; order 22.
   - A callee parameter with const above the pointee level (`S mk(char* const* p)`, `{ mk(&s), 1 }`) declines capture
     (the direct-brace wrapper's pointer spelling sites only spell pointee const): master and fix 1/0/1/2, clang 1/0/0/1.
     Leg 13615 pins it. Fix: spell every PointeeConstMask level on all wrapper pointer sites, then drop the decline.

## Deferred, out of scope

- Pre-existing: `Q(Q(mk(6)))` picks the outer copy ctor; clang is `6/1/0/0/0`, construct_at on the fix is `106/2/1/0/1`, and master is `1206`.

T224 review 1 notes: clang's rule fits all four multi-candidate oracle shapes - a variable is elided when
every return inside its scope, after its declaration, names it (no maintainer decision needed; it is
clang parity). (By-value argument temp extra move fixed by T231.)
- (T240 review 1, pre-existing; repros scratch/repro_keep/t240_rev/) base-class slot with a derived ctor
  `construct_at(bslot, D(3))`: 4/3, clang 3/2 (extra copy before the slice).
  `construct_at(ps, MkP(3))` with ps a CFlat struct pointer is refused "cannot assign a struct value to a pointer
  variable" (master too).
