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

## Landed (BRACEELIDE)

- BRACEPRV order rule: a later element reading a scalar or pointer stack local by bare name (or a
  construction from literals and such reads) no longer keeps an earlier element eager when no code
  that ran before took the local's address (IR uses are plain loads/stores, no enclosing loop, no
  goto) and earlier elements name it only as a direct by-value argument. `{ BS(k, 1), k }`,
  by-value parameters, `{ mk(k), k }`, `{ mk(BS(2).v), BT(t), 2 }`: clang counts. Legs
  Test/test_cpp_interop.cb 13616-13623.
- BRACECALL: a call whose arguments are calls or temporaries (`{ mk(g()), 1 }`, `{ mk(S(2).v), 1 }`,
  sole element `{ mk(g()) }`, leg 13632) is captured; `char* const*` parameters spell every const level on the
  wrapper and the callable, so leg 13615 is clang parity (1001). Legs 13624-13626.
- A call fed temporaries or nested calls initializing a CFlat struct field or a CFlat array element
  of a C++ class builds in the slot (`W w = { e = rb(CV(1)) }`, `CV[2] a = { rb(CV(1)), rb(CV(2)) }`,
  C++ callees alike): clang born 2 / 4, master 3 / 6 and value 2 / 23 for a mutable-copy class.
  Legs 13627-13631.

## Still open

1. Order rule declines, measured 2026-11-19 (clang 1001 for all; fix 1012):
   - `{ mk(1), side() }` (leg 13611 pins it) and `{ mk(BS(note(3)).v), note(4) }`: a later element
     with a side effect; parity needs the later element evaluated inside the wrapper in order.
   - the local's address was taken earlier (`int* pk = &k;`), the local is a global, or the brace sits
     in a loop (k declared in the loop body would be provable; not done).
2. Paren-aggregate `CM l = CM(S(5), 3);` with `struct CM { S c; int k = 9; };` (see cpp-paren-aggregate-prvalue-member-move.md).
3. A function-pointer parameter, `{ mkfp(f), 1 }` with `BS mkfp(int (*)(int))`: fix = master 1012, clang 1001
   (BRACEELIDE review 1).
4. A CFlat callee taking a C++ class by value as a C++ aggregate element, `Outer o = { rb(BS(1)), 1 }`:
   clang 2002, fix 2013 (the capture takes scalar and pointer parameters only).

## Deferred, out of scope

- Pre-existing: `Q(Q(mk(6)))` picks the outer copy ctor; clang is `6/1/0/0/0`, construct_at on the fix is `106/2/1/0/1`, and master is `1206`.

T224 review 1 notes: clang's rule fits all four multi-candidate oracle shapes - a variable is elided when
every return inside its scope, after its declaration, names it (no maintainer decision needed; it is
clang parity). (By-value argument temp extra move fixed by T231.)
- (T240 review 1, pre-existing; repros scratch/repro_keep/t240_rev/) base-class slot with a derived ctor
  `construct_at(bslot, D(3))`: 4/3, clang 3/2 (extra copy before the slice).
  `construct_at(ps, MkP(3))` with ps a CFlat struct pointer is refused "cannot assign a struct value to a pointer
  variable" (master too).
