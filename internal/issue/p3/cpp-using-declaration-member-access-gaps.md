# C++ using-declarations: re-exposed base members still refused

Found by the T99 reviews 2-4 (2026-10-10), pre-existing on master. T99 made a field re-declared by
`using Base::v;` in the queried class count as declared there (and take the using's access when the
hierarchy holds exactly one field `v`). These shapes clang++ -std=c++20 accepts are still refused:

- `using` in a BASE class re-exposing a private-base field, used through a further-derived class (clang 7).
- Two bases each with a field `v`, `using B::v;` selects B's (clang 13): the flattened layout keeps A's slot
  under the name and renames B's to `__hidden...`, so T99 does not touch the access (ambiguous name).
- A derived field shadowing plus `using` (clang 31), same reason.
- `using M::v;` where `v` is a METHOD of a public base while a private base has a data field `v`:
  `d.v()` (clang 23) - refused as the private field.
- A virtual base re-exposed by `using` (clang 13): flattening refuses virtual bases.
- Non-wrapper default-argument ambiguities omit the " = ..." default markers in the candidate list
  (wrapper candidates show them since T99).

Repros: scratch/repro_keep/t99/ (rev_t99_r3_<case>.cb + .cpp oracles + rev_t99_r3_cases.hpp,
rev_t99_r4_cases.hpp): base_using, select_base, shadow_using, method_call, virtual_base.

Fix direction: give each flat slot its FieldDecl identity (target of the UsingShadowDecl -> exact
base-subobject offset) so the name and access follow clang's lookup result, not the first slot of a name.
