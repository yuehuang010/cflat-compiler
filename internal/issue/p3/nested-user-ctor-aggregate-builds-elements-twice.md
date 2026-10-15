# Nested aggregate of user-ctor array elements builds every element twice

Found: T218 Opus review 1 (2026-10-14), pre-existing on master. `struct E { string s; E() { ... } ~E() {...} };`
`struct O { E[2] h; }; struct O2 { O[2] a; };` then `O2 v = { a = { { h = { x, x } } } };` runs every `E()`
twice and the first round leaks: all-braced elements give ct=8 dt=4 on master and after T218; the expression
form gives master ct=9 dt=4, T218 ct=8 dt=7. Probe: scratch/rev_t218_opus_probe.sh (T218 worktree).

Also seen (same review, pre-existing): a struct FIELD default `E[2] h = { gx, gx };` never runs `E()` (ct=0) -
check whether a user ctor must run before the default elements are copied in.

Fix direction: the outer aggregate's zero-fill/construct pass and the inner T129/T218 update route both
construct the inner elements; construct once (inner update path only) when the outer element is itself built
by an initializer list.
