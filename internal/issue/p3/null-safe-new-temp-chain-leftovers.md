# Null-safe chains on owned temps: pointer results still leak (by design)

T222 released the owned receiver of `?.` chains that end on a scalar: `(new T())?.next?.v`,
`(new T())->next?.v`, `look((new T())?.self())`, `make(4)?.v` now free once.

Still leaking, one allocation, zero dtors - the same rule as the `->` form:
- `(new T(6))?.pv()` and `(new T(6))->pv()`: the result is a pointer INTO the receiver. Freeing at the
  `?.` merge was a use-after-free (T106 r1); freeing at end of full expression is unsafe while the
  result is bound to a local or returned.
- `make(3)?.pv()`, `make(3)->pv()`: same.

Fix direction: release the receiver after the last consumer of the interior pointer, which needs a
lifetime view of the bound local. Until then the leak is the accepted behaviour; legs pin the value
only (T222 "pv result bound to local live").

T222 review 1 (pre-existing, same on `->`): `(new T(5))?.self()?.next?.v` / `mk(5)->self()->next->v`
leak the root (1 new / 0 dtors); `(move p)?.v` and `(move p)->v` leak the moved-out root.
