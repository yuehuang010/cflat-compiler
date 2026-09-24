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

## Status 2026-09-23

Not ruled. Maintainer wants a spike first: the memory-safety / lifetime view of `&ref()` vs
`ref()` at a return, and what each lowering means for LLVM IR optimization (aliasing metadata,
what the optimizer may assume about the returned pointer). Do not fix before the spike is read.

## Spike (2026-09-23)

Memo: scratch/refret_spike.md (probes under scratch/refret/). Measured: `&` adds no information
(identical IR at -O0 and -O2, CFlat sets no return attributes, PointsToAliasBorrow records only
that the address came from an alias value and is lost through a local). Field stores run the
other way: `h.r = &e;` accepted for every receiver including a frame-local one (dangles),
`h.r = e;` refused only for a class with a non-trivial ctor/dtor (type-based false positive).
Real gap: `return local_ref(lc);` with a local passed by reference to a free C++ function is
accepted and dangles. Recommendation: option (a) - `&` over a C++ reference result follows the
no-`&` receiver-kind gate, field store the same, and the gate also checks every argument that
is a local of the returning function, not just the receiver. Awaiting ruling.

## Ruling (maintainer, 2026-09-24)

Option (a) WITH the widening, approved. `&` over a C++ reference result carries no extra
meaning: `return &e;` and a pointer-field store `h.r = e;` / `h.r = &e;` go through the same
frame-rooted gate as `return e;`. The gate refuses when the receiver OR any pointer/reference
argument of the call is rooted in the returning frame (closes `return local_ref(lc);`).
`alias C*` stays the manual override. CFlat-native `alias` results (`&t.at()`) are unchanged.
Laundering through a local stays accepted (raw pointer lifetimes untracked). The ternary return
cell (`cpp-reference-results-in-ternary-refused-as-pointer.md`) follows the same rule per arm.
