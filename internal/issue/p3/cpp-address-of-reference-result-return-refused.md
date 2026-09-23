# The alias-escape gates refuse C++ reference results the conversion now accepts

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing.

## Summary

From a non-`alias` `C*` function, `return &cell();` (`C& cell()` returning a static) is refused by
the alias-escape return gate ("cannot return an 'alias' value 'cell'; it borrows storage it does
not own and would dangle") because `&` over an alias lvalue sets `PointsToAliasBorrow`. Since
fix/cpp-reference-return-at-return the no-`&` spelling binds the same address and is accepted
(refused only when the receiver is a frame-local object). The two spellings of one conversion
now disagree. Needs a maintainer ruling: either the `&` form follows the no-`&` rule for C++
reference results, or both stay as they are (`alias C*` return type accepts both).

## Repro

scratch corpus `rr_p12.cb` (refused), `rr_p01.cb` (accepted), `rr_q01.cb` (`alias` return accepts `&`).

The FIELD-store twin: `h.r = bref();` and `HR h = { r = bref() };` into a `R*` field, where
`B& bref()` returns a static of a NONTRIVIAL class, are refused by the alias-into-field gate
("cannot store an 'alias' value 'bref' into a field"), while the pointer LOCAL
(`R* r = bref();`) and, since the same fix, the field DEFAULT (`R* r = bref();` inside the struct)
bind it. A trivially copyable referent (`C* c` field, `h.c = cell();`) is accepted by all forms.
Repro: scratch corpus `rr_a03b.cb`, `rr_a18.cb` (refused), `rr_a17.cb` (accepted).
