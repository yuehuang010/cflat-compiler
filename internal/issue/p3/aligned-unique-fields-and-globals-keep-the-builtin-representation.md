# `alignas(0,N) unique T*` FIELDS and GLOBALS still keep the builtin pointer representation

Filed 2026-09-03, left over from a0f1a1f (remove the builtin unique remnant). Maintainer ruling
2026-09-03 was "remove the builtin path ENTIRELY"; the commit removed it for locals, parameters
and returns only. The agent brief told it to leave field-level `unique` untouched, so the ruling
is not yet fully landed. Not a bug: aligned fields free correctly today through the builtin path.

## What is left

A `unique` declarator that carries a folded alloc-alignment clause (`AllocAlignValue != 0`) skips
the `unique<T, ALIGN>` desugar and stays a raw pointer with `TypeAndValue::IsUnique = true`:

- struct/class field group: MainListener_Declarations.cpp ~2950-2957 ("Preserve the raw
  ownership carrier for the aligned-field remnant") and the sibling arms ~2990-3000.
- global declaration: MainListener_Declarations.cpp ~1008-1013 (`hasUniqueSpecifier &&
  declType.AllocAlignValue != 0`).
- interface fields (~1648-1654) are a SEPARATE, deliberate carve-out: they keep the contract
  marker because the wrapper would lose the ABI-level ownership agreement. Not in scope here.

The comment at ~2952 ("the wrapper cannot carry the allocation alignment needed by its
destructor") is STALE since a0f1a1f: `struct unique<T, int ALIGN = 0>` (cflat/core/unique.cb)
frees through `__delete_aligned` when `ALIGN > 16`, and locals already desugar
`alignas(0,64) unique T*` to `unique<T, 64>`.

Consumers that stay alive only for this remnant: the field-level `IsUnique` + `AllocAlignValue`
plumbing at LLVMBackend_OwnershipTemps.cpp ~1271-1290 / ~3382-3393, LLVMBackend_CodegenHelpers.cpp
~1051-1056, MainListener_Utilities.cpp ~100-130, and the alignas-field store guards in
MainListener_Declarations.cpp ~3035-3047 and ~7037-7130.

## Fix direction

Desugar aligned fields and globals to `unique<T, N>` exactly like locals (same
`FoldCompileTimeInt` path, both ParseDeclarationSpecifiers copies), then delete the two
`IsUnique = true` arms above and whichever alignment consumers go dead. Keep the interface-field
carve-out and its comment. Tests to re-shape (all field/global alignas + unique):
Test/test_basic.cb ~4962 and ~5117-5128, Test/errors/err_alignas_alloc_requires_unique.cb,
err_align_alloc_indirect_store.cb, err_align_brace_init_store.cb, err_align_alloc_mismatch.cb -
every expect_error must keep firing with equivalent wording (SpellType prints
`unique<T, 64>`), and the "alignas(0,N) pointer field must be unique" rule (~3035-3047) must
survive as a rule on the desugared type. Check `sizeof` assertions on structs with aligned
unique fields stay 8 per field.

Related: internal/plan/unique-ownership.md (status record), [[unique-library-type-direction]].
