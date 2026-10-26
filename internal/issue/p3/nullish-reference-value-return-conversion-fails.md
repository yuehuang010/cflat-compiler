# Nullish C++ reference value cannot convert to pointer return

Summary: A reference-valued right side of nullish coalescing is treated as a struct value instead of an address.
Repro: `return p ?? cpprefgate.cell();`.
Master vs expected: master reports `cannot assign a struct value to a pointer variable`; expected a pointer-compatible reference result.
Found: T423 r1 opus review, 2026-10-24 box, PRE-EXISTING
