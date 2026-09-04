# Static analysis: the delete form must match the allocation

Status: ON HOLD (maintainer, 2026-09-04): ruled, then held the same day - do not start any part of
this plan or its check-4 issue until the maintainer reopens it; the ruling may change.

## Ruling

`T* var = new T[10];` must stay legal and C-compatible: CFlat is an extension of C and a raw
pointer holding a heap array is part of that contract. The 2026-09-03 direction "block `new T[n]`
binding to a bare `T*` / desugar every site to `array<T>`" is withdrawn. The `array<T>` desugar
that landed for `array<T>` destinations and `auto` stays as an additive convenience.

What replaces it: static analysis that the `delete` applied to a raw pointer matches how it was
allocated, so the hidden-count machinery (`.raw_array_count` on `move T*`, `AllocatedByRawNewArray`
/ `RawArrayLength*` per-local facts) has a checker in front of it instead of a runtime slot
silently honoured by some consumers and ignored by others.

Clarification (same day): the `T*` from `new T[n]` IS the C-style pointer to the first
element - a plain address with C representation. It indexes, does pointer arithmetic and passes
to C functions exactly like a C array pointer. No fat pointer, no representation change, no
count inside the pointer value; a count exists only as a static fact on the binding or in the
existing `move T*` ABI slot. (An allocator-side header in front of the block would keep
`p == &p[0]` but is NOT ruled in; do not assume it.)

## Scope (to be refined before build)

Facts the front end already has: `AllocatedByRawNewArray`, `RawArrayLength*` on the local
(decl-init and assignment), the `move T*` count ABI across calls, `unique<T>` adoption sites.
Checks to add, cheapest first:

1. `delete p` (scalar form) where `p`'s provenance is a `new T[n]` in the same function ->
   error "'p' holds a heap array of 'T'; use 'delete[] p' / 'delete[n] p'" (and the reverse:
   `delete[] p` / `delete[n] p` on a scalar `new T`).
2. `delete[n] p` where `n` is a constant and the allocation count is a constant that differs.
3. Provenance through a `move T*` parameter: the count slot is already there; the checker's
   job is to make explicit `delete p` on such a parameter route through the counted destructor
   (landed as e2c4a1e for the scope-exit path; this is the explicit-delete sibling).
4. Adoption into `unique<T>` (`p2/unique-field-heap-array-through-move-param`): a static reject
   where provenance is precise, and the runtime trap at the core ctor/reset when it is not.
   Both options are back on the table under this ruling.
5. Escapes the analysis cannot see (stored into a struct field, returned as bare `T*`) stay
   legal and unchecked, as in C. Do not reject them; document the boundary.

Prior art: clang-tidy `cppcoreguidelines-owning-memory` / `bugprone-mismatched-delete`;
MSVC /analyze C6283 ("delete[] mismatch"). Both are intraprocedural with a declared-owner
annotation; the `move T*` count ABI is cflat's equivalent of the annotation.

## Acceptance

Existing `Test/test_core.cb runRawCount*` legs keep passing; the 800 `T* p = new T[n]` sites in
core / Test / example compile unchanged; new `Test/errors/err_delete_form_mismatch.cb` covers
checks 1-2; check 4 closes the p2 issue.
