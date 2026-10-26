# Mixed C pointer and C++ reference return joins fail

Summary: CFlat rejects pointer/ref alternatives in returns through `??` and `?:`.
Repro: `return p ?? &cpprefgate.cell();` or `return p != nullptr ? p : &cpprefgate.cell();`.
Master vs expected: master reports `cannot return an 'alias' value` for the C* parameter joined with a C++ reference result; expected a compatible pointer result.
Found: T423 r1 opus review, 2026-10-24 box, PRE-EXISTING
