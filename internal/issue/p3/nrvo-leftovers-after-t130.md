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

1. `construct_at(slot, T(f(x)))` with a nested call argument (and the declaration `C c = T(f(x))`)
   still builds a temporary and moves it: clang 1 / 0, we 2 / 1. The single-call arming rule refuses
   any nested call; lifting it needs a proof that the inner call cannot take the armed slot.
2. `new S(mk1())` with S a CFlat struct holding a C++ field: "no overload of 'S'" on master and
   after T224 (clang accepts and elides). Copy/move constructor surface for CFlat structs is a
   language decision; not started.

T224 review 1 notes: clang's rule fits all four multi-candidate oracle shapes - a variable is elided when
every return inside its scope, after its declaration, names it (no maintainer decision needed; it is
clang parity). Pre-existing: `C x = (mk1(7));` copies (107 vs clang 7). (By-value argument temp extra
move fixed by T231.)
- (T240 review 1, pre-existing; repros scratch/repro_keep/t240_rev/) an OPERATOR result as the ctor argument,
  `construct_at(s, T(a + b))`: 2 ctors / 1 dtor after T240 (master 3/2), clang 1/0 - same family as T(f(x)).
  Base-class slot with a derived ctor `construct_at(bslot, D(3))`: 4/3, clang 3/2 (extra copy before the slice).
  `construct_at(ps, MkP(3))` with ps a CFlat struct pointer is refused "cannot assign a struct value to a pointer
  variable" (master too).

## Ruling 2026-10-15 (`new S(mk1())`): brace init only
CFlat structs get no paren/copy-constructor init surface: `new S(mk1())` stays refused, with a steer to the brace form
`new S{ c = mk1() }` (improve the message from "no overload of 'S'"). The extra copy from `Cxx x = (mk1());` (107 vs clang 7)
is a bug: parentheses never add a copy - fix it.
