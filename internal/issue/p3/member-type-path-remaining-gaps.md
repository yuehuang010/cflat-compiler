# Member type paths (`Spec.name`): CFlat `static` field ignored, unsigned `if const` statics

T37 (5837c4fa, 2026-10-05) made `Spec.name` name C++ member typedefs / nested types and CFlat struct `using`
members in every type position. MEMTYPE fixed the member-template path and non-generic closure aliases;
GENCB fixed closure aliases on generic owners; STATMEM fixed C++ static data members as value template
arguments. Two items remain.
Member-template and closure-alias coverage: `Test/test_generics.cb`, `Test/test_cpp_interop_template.cb`,
and `Test/library/cpp_t37_member_types.hpp`.

## 1. `static` on a CFlat struct field is silently ignored

The `static int value = 7;` declaration is accepted inside a CFlat struct, but it is stored as an
ordinary per-instance field: assigning `a.value` does not change `b.value`. The owner-qualified
read `Spec.value` is refused with `'value' does not name a value here. If it is a method, call it: 'value()'.`
The Structs section of `doc/LANGUAGE.md` documents fields and member functions but no static data
storage. CFlat static data members are therefore a feature, not a remaining value-resolution gap;
they are out of scope for this fix.
Defect side: accepting `static` and storing a per-instance field is a silent miscompile; until static
storage is designed (needs a ruling - p4 surface), `static` on a CFlat data member should be refused
with a located error.

## 2. `if const` refuses a C++ static above the int32 range

`if const (cppt.U<int>.v > 0)` with `static constexpr unsigned long long v = 1ull << 62;` (or
`unsigned v = 4000000000u;`) is refused "'if const' condition must be a compile-time constant
expression"; clang takes the branch. Cause: the scanner fallback in EvalIfConstConstant accepts only
values in InScannerInt32Range (cflat/MainListener_Support.cpp ~1950). Refuses, never miscompiles.
A static at or above 2^63 (`~0ull`) is deliberately not constant (FoldableStaticValue) and must stay
refused until values carry signedness. Fix direction: carry 64-bit + signedness through the scanner
fold. Repro: header `namespace cppt { template<class T> struct U { static constexpr unsigned long long v
= 1ull << 62; }; }`, then `int main() { if const (cppt.U<int>.v > 0) { return 1; } return 0; }`.
