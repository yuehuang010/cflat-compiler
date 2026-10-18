# Field default overridden by a nested list copies then destroys the default

Found: T312 (2026-10-16), pre-existing on master. Pure CFlat, balanced (no leak, no double destroy).
`struct E { string s; int w = 0; E() { ct++ } ~E() { dt++ } };` with `E gx = default;`,
`struct FD { E[2] h = { gx, gx }; };`, `struct R { FD f; int k = 1; };`.
`R r = { f = { h = { gx, gx } } };` destroys two copies inside the scope (dt=2 before scope exit).
clang++ -std=c++20 never makes or destroys those default copies (dt=0 inside, dt=2 after).

Identical output on master and on the T312 branch (checked 2026-10-16).

Fix direction: when a named field override replaces a braced field default, do not copy the default
first. The override path (EmitFieldInitializer nested array arm) should replace the field without
building the default. Copies are not counted by a user ctor, so this shows only in destructor counts.

## Element-expression default-then-replace (T312 review 1, 2026-10-16)
An element initialized from an EXPRESSION is first default-constructed, then replaced (ct/dt, inside scope -> after):
- one level `O v = { h = { x, x } };`: CFlat 2/2 -> 2/4 (master and T312 same), clang 0/0 -> 0/2;
- nested `O2 v = { a = { { h = { x, x } } } };`: master 8/2 -> 8/6, T312 4/2 -> 4/6, clang 2/0 -> 2/4;
- `h = { move y, {} }` with a unique<R> field: 4 R allocations vs clang 3 (balanced, live 0).
Leg Test/test_cpp_interop.cb 12936 pins the T312 count (4/6) - update it when this is fixed.
Fix direction: construct an expression element directly from its initializer (copy/move) instead of default + assign.
Probe: scratch/repro_keep/wt_t312/rev_t312_opus_probe.sh (cases L_expr_nested, K_expr_1lvl, D2_unique_move).
