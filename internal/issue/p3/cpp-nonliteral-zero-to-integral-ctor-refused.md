# C++ ctor call with a non-literal integral zero refused where clang picks the integral overload

From T87 review 2 (2026-10-10), pre-existing on master. A class with (short, const int&) / (int*, const double&)
style overloads: `new C('\0', 1.5)`, `new C(false, 1.5)`, enum zero, `1-1`, and a variable holding 0 are all
refused by CFlat; clang++ -std=c++20 picks the integral constructor (returns 11) because only a literal 0
is a null pointer constant. T87 limited null-constant spelling to literal zeros, so these fall back to the
old refusal path.

## Repro
scratch/repro_keep/t87/ (rev_t87_* probes; char/false/enum/expr/variable zero cells)

## Fix direction
In the scalar-reference mirror (LLVMBackend_CInterop.cpp ~15375), non-null-constant zeros should take the
named-argument spelling and let clang rank them; find why they are refused before reaching the mirror.
