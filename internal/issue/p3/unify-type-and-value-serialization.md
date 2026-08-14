# Unify the two `TypeAndValue` serialization schemas

Filed 2026-08-13 during the integrated Q01-Q15 review.

## Summary

`TypeAndValue` is serialized independently in two places:

- `SerializeTav` / `DeserializeTav` in `LLVMBackend.cpp` for the core bitcode cache.
- `TvToJson` / `TvFromJson` in `LLVMBackend_StateAndImports.cpp` for C-header cache data.

The Q10/Q15 changes had to add function-pointer return ownership, aliasing, and parameter
allocation alignment to both implementations. The schemas already differ in field coverage and
key spelling: the core serializer includes resolved type keys and function-pointer pointer depths,
while the C-header serializer does not.

Some differences may be intentional because the consumers need different subsets. Encoding those
differences as two handwritten copies makes that impossible to distinguish from an omission. Every
new semantic field creates another warm-cache correctness risk.

## Simplification direction

Define one canonical schema adapter for the common semantic fields and let each cache explicitly
request or append its format-specific fields. Reasonable shapes include a shared field visitor or a
single neutral DTO converted to `llvm::json` and `nlohmann::json` at the boundary.

Acceptance criteria:

- There is one declaration of every serialized `TypeAndValue` semantic field.
- An intentional cache-specific omission is named and documented at the call site.
- Existing cache data remains backward compatible, or the cache version/key is bumped so stale
  entries are rejected.
- Cold-cache and warm-cache runs of the existing error suite produce the same diagnostics.

