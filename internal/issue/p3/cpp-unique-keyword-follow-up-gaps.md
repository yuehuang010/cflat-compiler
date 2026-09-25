# `unique` on a C++ class (std::unique_ptr, R5): follow-up gaps

Bucket: p3 (compile errors on valid code, no wrong code). Found 2026-09-24 by the review of the
R5 change. Each gap also exists for the SPELLED `std.unique_ptr<W>` form before R5; the keyword
just makes them easier to reach.

## 1. (fixed beb49cea) A prvalue into a CFlat by-value unique parameter

Move-constructs now; bridge legs 3590-3599.

## 2. (fixed c1ed0801) `list<unique W*>` over a C++ class

## 3. (fixed 9bedb16f) Diagnostics spell the lowered type

## 4. Assigning a `unique` C++ class value to `int` gives an unrelated message

`unique cpuq.Widget* a = new cpuq.Widget(1); int n = a;` reports "cannot cast an aggregate
value - a fixed array decays to a pointer to its first element" (found by the gap-3 probes,
same on the spelled std.unique_ptr form). Should be a plain type-mismatch naming both types.
