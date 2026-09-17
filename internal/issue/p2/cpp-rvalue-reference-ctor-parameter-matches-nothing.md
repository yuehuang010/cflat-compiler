# A C++ constructor with a `T&&` parameter never matches a `move x` or prvalue argument

Found 2026-09-17 during fix/cpp-ctor-ref-param. Pre-existing on master.

## Repro

`struct Taker { Taker(Cnt&& c); };` then `Taker t = Taker(move x);` or `Taker(make_cnt())` -> "has no constructor whose parameter types match these arguments", before and after the ctor-ref fix.

## Notes

SelectCxxConstructor::compatible admits only lvalue references (`IsAlias && !IsRvalueRef`) on purpose: admitting rvalue refs made `std::optional<int>(in_place_t, int&&)` a candidate for an int argument and the run segfaulted in `__optional_destruct_base` (raw integer used as an address). Any rvalue-ref rule must materialize scalars into storage before passing the address.

## Fix direction

Admit `T&&` for a `move` argument or a prvalue only (never a plain lvalue), materializing scalars; add a fixture leg.

Suggested bucket: p2.
