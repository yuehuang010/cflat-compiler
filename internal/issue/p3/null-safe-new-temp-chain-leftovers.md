# Null-safe chains on owned temporaries still leak in three shapes

PRE-EXISTING (T106 review 1; reproduce on master before and after T106). T106 frees `(new T())?.f`
when the member right after `?.` is a scalar field. Still leaking (one new, zero dtors):
- Nested chain `(new T(4))?.next?.v` (also `(new T(4))->next?.v`).
- A `?.` method call on a new temp, e.g. `look((new T(8))?.self())` or `(new T(6))?.pv()`: the
  receiver is kept on purpose because the call may return a pointer into it (freeing at the `?.`
  merge was a use-after-free in T106 r1). The `->` form `->self()` frees once.
- An owning call result as receiver: `make(4)?.v` (the `->` form frees).
Fix direction: release the receiver at the end of the FULL expression (after every consumer of a
possibly interior result), not at the `?.` merge; then scalar and non-scalar tails share one path.
Repros: scratch/repro_keep/t106/ (rev_t106 probes).
