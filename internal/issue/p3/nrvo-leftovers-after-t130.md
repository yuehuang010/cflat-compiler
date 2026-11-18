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

1. Brace init of a C++ class from one same-class constructor prvalue (`S x = { S(7) };`) is built in place since
   BRACEPRV (the brace wrapper spells `S{ S(7) }`, clang elides). BRACECALL fixes same-class CALL prvalues in a
   single-element local brace init: `S x = { mk(7) };`, `{ (mk(7)) }`, `S x { mk(7) }`, CFlat and method callees,
   and a call ternary now use the declaration's sret / ternary destination path (1 ctor / 0 move / 1 dtor).
   Still open: `Outer o = { mk(3), 1 }` into an aggregate member. The aggregate brace wrapper parses the call in
   CFlat and passes its materialized value as `__cflat_arg_N__`; it has no path to aim that call's sret at the
   member slot. Fixing this needs the resolved call deferred and spelled in the generated C++ wrapper (or a new
   member-wise aggregate lowering). Master behavior remains 1 ctor / 1 move / 2 dtors for this shape.
   Also open (BRACEPRV order rule): a constructor element is deferred only when every LATER element is a literal or a
   construction from literals, and no earlier element left a CFlat class temp. So `int k = 4; Outer o = { BS(k, 1), k };`
   (braceprv.hpp shapes) still moves: 1 ctor / 1 move / 1 dtor in scope, clang 1/0/0. Lifting it needs a proof that the
   later element cannot observe the constructor (a local read the constructor may write through a pointer).
2. Paren-aggregate `CM l = CM(S(5), 3);` with `struct CM { S c; int k = 9; };` (see cpp-paren-aggregate-prvalue-member-move.md).

## Deferred, out of scope

- Pre-existing: `Q(Q(mk(6)))` picks the outer copy ctor; clang is `6/1/0/0/0`, construct_at on the fix is `106/2/1/0/1`, and master is `1206`.

T224 review 1 notes: clang's rule fits all four multi-candidate oracle shapes - a variable is elided when
every return inside its scope, after its declaration, names it (no maintainer decision needed; it is
clang parity). (By-value argument temp extra move fixed by T231.)
- (T240 review 1, pre-existing; repros scratch/repro_keep/t240_rev/) base-class slot with a derived ctor
  `construct_at(bslot, D(3))`: 4/3, clang 3/2 (extra copy before the slice).
  `construct_at(ps, MkP(3))` with ps a CFlat struct pointer is refused "cannot assign a struct value to a pointer
  variable" (master too).

## Open after BRACECALL (2026-11-15)
- The call-element residue is now only the aggregate-member shape in item 2; local same-class call elements land
  through the declaration slot path.
