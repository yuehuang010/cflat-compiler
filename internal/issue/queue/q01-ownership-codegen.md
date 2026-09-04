# q01 - Ownership codegen: counted move pointers, slot moves, unique remnant

Originally five items in the move / unique codegen paths; members 10-12 added 2026-09-03 from review spin-offs. Two share a root cause (a consumer drops the
hidden `.raw_array_count` a `move T*` carries); the rest are the same area and the same files
(`LLVMBackend_OwnershipTemps.cpp`, `LLVMBackend_Overloads.cpp`, `MainListener_Expressions.cpp`,
`MainListener_Declarations.cpp`).

## Members

| Order | Item | Status | Shape |
|-------|------|--------|-------|
| 1 | `p2/delete-of-move-param-ignores-raw-array-count` | LANDED 2026-09-03 `e2c4a1e` (3 fix rounds: nbody/lu_bench crash from element-field moves inheriting the base count, then a stale count on a reassigned parameter; opus round-2 review CLEAN) | `delete p` on a `move T*` param takes the scalar dtor path; factor the count branch out of scope-exit cleanup and reuse it. Positive leg: `runRawCount*` in `Test/test_core.cb`. |
| 2 | `p2/unique-field-heap-array-through-move-param` | RULING (option 4 recommended) | adoption into `unique<T>` via `reset(move T* p)` drops the count. Runtime trap at the core ctor/reset call emission when `count >= 0`. Static forms measured: 0 true / 5 false positives. |
| 3 | `p2/move-out-of-slot-nulls-the-slot-after-the-call-returns` | LANDED 2026-09-03 `53a176e` (3 review rounds: codex found an indirect-call interface regression, opus found a `default`-argument false rejection) | null store for a `move <slot>` argument is emitted post-call; a callee that reseats the slot is clobbered. Emit the store BEFORE the call for non-local sources. Positive leg in `Test/test_move.cb`; one sentence in `doc/LANGUAGE.md` slot rule. |
| 4 | `p3/aligned-unique-fields-and-globals-keep-the-builtin-representation` | LANDED 2026-09-03 `cb3f71b` (opus round-1 review: no correctness defect; two coverage legs added; follow-up `p3/explicit-unique-align-spelling-rejects-plain-new` filed) | desugar `alignas(0,N) unique T*` fields/globals to `unique<T, N>` like locals; delete the two `IsUnique = true` arms and dead alignment consumers; keep the interface-field carve-out. Five `err_align*` tests must keep firing. |
| 5 | `p2/move-return-temp-ignores-raw-array-count` | LANDED 2026-09-03 `d7af90d` (opus round-1 review CLEAN) | owned pointer TEMPORARIES from a `move T*` return (`delete make()`, end-of-expression temp free) take the scalar destructor; route `EmitOwnedPtrTempFree` through `EmitOwningPtrDestructor` with the registered call-result count. |
| 6 | `p2/move-param-reassign-drops-alloc-alignment` | LANDED 2026-09-03 `4223c66` as a REJECTION, not a record: recording alignment on the binding was unsound under conditional reassignment (3 review rounds) | `SetVariableAllocAlignment` walks only `namedVariable`; a reassigned `move T*` param loses its `alignas(0,N)` fact and is freed unaligned. Seven-line mirror of `e2c4a1e`'s `functionArgument` lookup plus legs in `Test/test_core.cb`. |
| 7 | `p2/tracked-aligned-local-keeps-alignment-after-plain-move-in` | LANDED 2026-09-03 `e04729c` (opus round-1 review CLEAN) | decl-init-inferred aligned local receives an ordinary block via `move`; `std::max` merge keeps 64, free is aligned on both paths. Remaining direction of the alignment rule; extend `RejectLocalAllocAlignMismatch`. |
| 8 | `p2/alloc-alignment-provenance-remaining-holes` | LANDED 458c6a9 | NamedVariable::AllocAlignKnown provenance set at the move site (field/element/tracked-new/owning-view/parameter sources); store-side door for unclaused fields, elements and deref slots; core unique<T,ALIGN> raw slot exempt as the authority. Spun off: `p3/alloc-alignment-unclaused-global-pointer-store-unchecked` (also records the `&alignedLocal` launder). |
| 9 | `p3/consolidate-named-variable-borrow-provenance` | REFACTOR, after 10-12 | fold the parallel `Borrows*` / `Bond*` / provenance field groups on `NamedVariable` into one value object; migrate owned-string + owned-element first, behaviour unchanged. |
| 10 | `p3/alloc-alignment-unclaused-global-pointer-store-unchecked` | READY (spin-off of member 8) | add the GLOBAL destination to the `RejectFieldAllocAlignMismatch` door in ParseAssignmentExpression; optional address-of door closes the `E** pp = &aligned` launder in the same rule. Legs in err_align_alloc_mismatch.cb + an accept leg for a declared `alignas(0,64) E*` global. |
| 11 | `p3/explicit-unique-align-spelling-rejects-plain-new` | READY (spin-off of member 4) | seed `pendingInitAllocAlign` from the `unique<T, N>` template argument as the sugar path does (declaration + assignment doors); reword the mismatch message to name the declared type. |
| 12 | `p3/mixed-owning-borrow-pointer-ternary-join-leaks-new-arm` | READY (ruling 2026-09-03 covers it) | extend `RejectMixedOwnershipTernaryJoin` to pointer joins where one arm is `new` / `move` / owning result and the other a borrow. Measure blast radius (`? new ` / `: new ` in Test/ and example/) and check LANGUAGE.md's pointer-join text first. |

## Shared root cause (members 1, 2, 5, 6)

`move T*` is a COUNTED owning pointer at the ABI level (`ParameterCarriesRawArrayCount`,
`.raw_array_count` slot, honoured by scope-exit cleanup and forwarding). Only two consumers ignore
the slot: explicit `delete` and `unique<T>` adoption. Fix both against the same slot; do not
re-open the "downgrade `move T*` to bare `T*`" model (recorded as a capability removal needing its
own ruling).

## Sequencing

Members 10, 11, 12 ship as ONE worktree (`fix/align-doors`) in FULL mode: they are disjoint
(assignment door / init alignment seed / ternary join) but each adds or widens a rejection, so
each needs its accept-set; one fix agent builds all three matrices, one review loop covers the
combined diff. 9 after they land. Original order for the record: 1 then 2 (2 reuses the count argument 1's fix touches). 3 is independent and can run in parallel
(disjoint: `CreateOverloadedFunctionCall` argument-side move vs `ParseDeleteExpression`). 4 after
1-3 land. 5 after 4, since 4 deletes some of the fields 5 would migrate.

## Constraints

- Explicit `move x` nulls the source and leaves it readable-as-null - member 3 changes WHEN, not
  whether ([[explicit-move-nulls-source]]).
- Unknown ACCEPTS for the guard family; a runtime probe is the sanctioned shape for what a static
  rule cannot separate (fix-issue-lessons digest).
- Both `ParseDeclarationSpecifiers` copies for member 4.

## Adjacent

- `p2/delete-borrow-via-named-local` (deferred): the container-element flavour of provenance
  through a parameter; NOT a member, needs interprocedural work.
- `p3/mixed-owning-borrow-pointer-ternary-join-leaks-new-arm` is member 12 (the struct join landed 8201425).
- `internal/plan/unique-ownership.md` status record.
