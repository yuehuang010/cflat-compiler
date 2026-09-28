# Spelled-type C++ class locals make copies/moves C++ elides

Measured on master c7d09afe against clang++ -std=c++20 (probe corpus scratch/acp/ in the
fix/auto-copy worktree, header acp.h, class S with ctor/copy/move/dtor counters). Values and
destructor pairing are correct - only extra constructions:

- `int g(r2.S s) { r2.S y = s; ... }` (not the last use): copy 3 vs C++ 2 - the bare-identifier
  branch of `TryDeclareForeignCxxLocal` routes a by-value parameter through
  `RequestCxxVariadicConstructor(copyInit)` and the wrapper takes the class by value. At the
  last use: copy 2 + move 1 vs the `auto` spelling's copy 1 + move 1.
- `r2.S x = c ? r2.S(1) : r2.S(2);` and `c ? a : r2.S(5)` / `c ? r2.S(5) : r2.ref()`: move 1
  where C++ builds in place - a `T(args)` arm is an owned temporary, not a published
  `lastCxxRetTemp_`, so `moveCxxTernaryArmIntoDecl` moves instead of retargeting. The `auto`
  spelling elides these (publishCxxCtorTemp in ParseTernaryBranches, `auto` arms only).
- `r2.S x = move hp->s;` is refused ("needs a plain variable as its source"); the `auto`
  spelling move-constructs as C++ does.

Fix direction: give the typed path the same treatment (publish `T(args)` arm temporaries for
declaration ternaries; copy a parameter source with EmitCxxCopyOrMoveConstruct directly).

## Also (review round 1, 2026-09-27)

- Nested construction `k.S w = k.S(k.S(6));` (and the `auto` spelling): copy 1 + one extra
  destruction vs C++'s single construction - the inner `T(args)` temporary is copied into the
  outer constructor's parameter instead of being elided. Same on both spellings.
- `auto z = *op;` on `std::optional<S>` (probe scratch/rev_acp/rev_acp_d3b.cb): the operator*
  result (C++ `S&`) is typed as a POINTER, so `auto` deduces `r2.S*` - a borrow of the
  optional's payload, no copy ctor, no dtor. C++ copies. The spelled `r2.S z = *op;` is refused
  ("cannot initialize C++ class"). Root: the `*` overload path (ParseUnaryExpression ->
  TryUnaryOperatorOverload) keeps `lastCallReturnType` with Pointer set, so
  PrepareAliasCallResult never turns the reference into lvalue storage; `op.value()` (a named
  member returning `S&`) is typed as an alias value and copies correctly.
