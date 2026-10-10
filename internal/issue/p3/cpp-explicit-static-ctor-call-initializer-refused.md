# Explicit-type static local initialized by a C++ constructor call is refused

Found by T62 review 1 (pre-existing, master same).

## Repro
Header: scratch/repro_keep/t62_rev/rev_t62_trk.hpp (class r62::Trk with a pointer field, counting ctor/dtor).
```cflat
int f(int a) { static r62.Trk g = r62.Trk(a); return g.v; }
int h(bool c, int x, int y) { static r62.Trk g = c ? r62.Trk(x) : r62.Trk(y); return g.v; }
```
Both are refused with "cannot copy ... shallow-copied pointer/view field". clang++ -std=c++20 accepts:
the prvalue initializes the static in place (1 made, 0 copies, 0 moves) on the first call only.
`static auto g = r62.Trk(a);` (T62) is the deduced spelling of the same case.

## Fix direction
Route the explicit-type static path through the same in-place construction the T62 fix uses for
`static auto` prvalues (construct directly into the `.static.` global inside the guarded init block).
