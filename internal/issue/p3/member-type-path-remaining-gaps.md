# Member type paths (`Spec.name`): CFlat `static` field ignored, unsigned `if const` statics

T37 (5837c4fa, 2026-10-05) made `Spec.name` name C++ member typedefs / nested types and CFlat struct `using`
members in every type position. MEMTYPE fixed the member-template path and non-generic closure aliases;
GENCB fixed closure aliases on generic owners; STATMEM fixed C++ static data members as value template
arguments. Items 1 and 2 remain; item 3 moved to p4.
Member-template and closure-alias coverage: `Test/test_generics.cb`, `Test/test_cpp_interop_template.cb`,
and `Test/library/cpp_t37_member_types.hpp`.

## 1. `static` on a CFlat struct field is silently ignored

The `static int value = 7;` declaration is accepted inside a CFlat struct, but it is stored as an
ordinary per-instance field: assigning `a.value` does not change `b.value`. The owner-qualified
read `Spec.value` is refused with `'value' does not name a value here. If it is a method, call it: 'value()'.`
The Structs section of `doc/LANGUAGE.md` documents fields and member functions but no static data
storage. CFlat static data members are therefore a feature, not a remaining value-resolution gap;
they are out of scope for this fix.
RULED 2026-11-18 (maintainer): IMPLEMENT shared storage with the C++ / C# meaning - one storage per struct (per
instantiation of a generic struct), read and written as `Spec.value` and through an instance, initializer run once.
No interim refusal: the spelling parses and both parents give it this meaning, so the fix is the feature, not an
error. Rejected: refuse-until-designed (the main session's pick) and keep-per-instance. Storage follows the globals
ruling (owning types legal, no exit-time destruction). Test consequence: `struct Spec { static int value = 7; int x = 0; }`
with `a.value = 9` reads `b.value == 9` and `Spec.value == 9`, `sizeof(Spec) == 4` (master: 7, refused, 8); any
new StructData field goes through the `--init` cache round-trip in the same change.
Plan: `internal/plan/cflat-static-fields.md` (phases, open questions). Until phase 1 lands the silent
per-instance storage stays.

## 2. `if const` refuses a C++ static above the int32 range

`if const (cppt.U<int>.v > 0)` with `static constexpr unsigned long long v = 1ull << 62;` (or
`unsigned v = 4000000000u;`) is refused "'if const' condition must be a compile-time constant
expression"; clang takes the branch. Cause: the scanner fallback in EvalIfConstConstant accepts only
values in InScannerInt32Range (cflat/MainListener_Support.cpp ~1950). Refuses, never miscompiles.
A static at or above 2^63 (`~0ull`) is deliberately not constant (FoldableStaticValue) and must stay
refused until values carry signedness. Fix direction: carry 64-bit + signedness through the scanner
fold. Repro: header `namespace cppt { template<class T> struct U { static constexpr unsigned long long v
= 1ull << 62; }; }`, then `int main() { if const (cppt.U<int>.v > 0) { return 1; } return 0; }`.

## 3. A fixed-array bound that is not a compile-time constant is silently one element

MOVED 2026-11-18: the bug (compiler error until the feature is built) is
`internal/issue/p3/non-constant-array-bound-silently-one-element.md`; the feature (C VLA, optional) is
`internal/issue/p4/runtime-sized-arrays-vla.md`.
