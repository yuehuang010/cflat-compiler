# Writing a `mutable` member of a const C++ object is refused (clang accepts)

Filed 2026-10-14 from the T168 Opus r2 review (PRE-EXISTING on master and the T168 branch).

## Repro
C++ header: `struct KM { mutable int m = 0; int n = 0; }; constexpr KM k_km{};` (or a `const KM` global / const-receiver
member function). CFlat `k_km.m = 5;` -> refused as a write to a const object. clang++ -std=c++20 accepts (`mutable` members
are writable through a const object). Repro: scratch/repro_keep/t168_rv2/ (copied from the T168 worktree scratch/t168_rv2/src/).

## Direction
The const-referent write refusal (RefuseCxxConstReferentWrite and the field-address const mark) should exempt a FieldDecl with
isMutable(); harvest the flag with the record fields. Note constexpr storage may be placed in read-only memory by LLVM - a
`mutable` member makes clang emit the object as non-constant; check the global's emission matches.
