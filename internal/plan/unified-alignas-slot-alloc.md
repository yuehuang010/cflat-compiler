# Unify slot + allocation alignment into `alignas(slot, alloc)`

Status: DONE (2026-07-14). Supersedes `internal/issue/no-aligned-raw-buffer-field.md`.

## As-built notes

Landed the unified positional `alignas(slot, alloc)` and finished the RETURN escape. Verified on
macOS arm64: `./test.sh Release` = 183 passed / 0 failed / 8 skipped; scratch Matrix + return-escaping
repro run with every aligned block 64-aligned and freed via `__delete_aligned` (confirmed in `--out-lli`:
`llvm.assume ... "align"(ptr, 64)` inside the returning function, caller frees through
`___delete_aligned_void_U8Ptr_`); `--init` then recompile confirms the `"aa"` bitcode round-trip holds.

- Syntax: `alignmentSpecifier : 'alignas' '(' (typeName | constantExpression) (',' constantExpression)? ')'`.
  `align_alloc` keyword + `allocAlignmentSpecifier` rule + its suffixes on `initDeclarator` /
  `parameterDeclaration` / `newExpression` deleted. arg2 parsing localized in `ParseAllocAlignArg`
  (a named form can hook in there later without touching callers).
- Escapes now covered: FIELD, MOVE-PARAM, and RETURN (all three). arg2 -> `AllocAlignValue` in BOTH
  `ParseDeclarationSpecifiers` copies (codegen folds+validates; ForwardRefScanner best-effort integer).
- Inbound channel: one-shot `pendingInitAllocAlign`, set from the target's `AllocAlignValue` before a
  DIRECT `new` initializer/RHS/return (gated by `AsDirectNew`, a single-child parse-tree descent);
  consumed+cleared in `ParseNewExpression`. RETURN result channel: `lastCallReturnsAllocAlign`, set from
  the callee return-type `AllocAlignValue`, consumed in `ParseDeclaration`.
- Safety boundary: inference reaches only direct decl-init / direct assignment / direct return. Indirect
  `new` (ternary/cast/call-arg) is NOT inferred; the receiving decl errors "cannot infer allocation
  alignment here", and a call-arg `new` into a `move alignas(_,N)` param is caught by the move-param check.
- Deviation from plan: chose POSITIONAL (per the task), not the plan's named-optional recommendation.
  Kept a bare `alignas(0)` (no arg2) an ERROR ("value must be positive") so `err_alignas_zero` still
  holds; only `alignas(0, N)` treats arg1 `0` as the natural-slot sentinel. On a `new`, a 1-arg
  `alignas(N)` still means the ALLOCATION alignment (back-compat); a 2-arg form takes arg2.

Promotes the shipped `align_alloc(N)` keyword (field + move-param escapes, landed) into a single
unified `alignas` specifier that also closes the deferred return-type escape. Captures the design
converged on in review; the ergonomics variant (positional vs named args) is the one open decision.

## Background: what already shipped (the `align_alloc` work)

A struct could not OWN a raw over-aligned buffer (e.g. a flat 64-byte-aligned `double[]`). Neither
mechanism sufficed: type-level `alignas` cannot align a primitive without wrapping it (changing the
element type / stride); per-site `new T[n] alignas(N)` gives the flat aligned buffer but the
alignment is deliberately NOT in the type, so it could not escape a local into a field / param /
return (compile error, previously silent heap corruption - see the fixed aligned-new-field-escape).

The landed fix introduced a NEW keyword `align_alloc(N)`, DISTINCT from `alignas`, on both `new` and
on a pointer/array-view declaration, so the owner and the allocation agree and the free site recovers
the alignment. Covered escapes: FIELD (done), MOVE PARAM (done), RETURN (deferred - still a clean
parse error). This plan replaces that keyword with a unified `alignas` and finishes RETURN.

## Three distinct notions of alignment (the whole reason for the confusion)

1. TYPE alignment - `struct alignas(64) Chunk` - rides in the type IDENTITY (changes ABI align +
   sizeof), travels everywhere for free.
2. SLOT alignment - `alignas(64)` on a field/variable BINDING - aligns that one slot's storage
   (the field's position in the struct, or a local's stack alloca). NOT part of the type identity.
3. ALLOCATION alignment - the alignment of the heap BLOCK a `new[]` returns. Emphatically NOT part
   of the type: the LLVM type of `double[]` is byte-identical with or without it, sizeof/stride
   unchanged, two `double[]` with different alloc-align are the SAME type.

Key fact for this design: allocation alignment is a property of the VALUE / BINDING, carried on
`TypeAndValue` as an annotation - NOT of the type. So "`new` already knows the type" does NOT hand
`new` the alignment; it must be threaded from the target declaration's `TypeAndValue` explicitly.

## Existing machinery (reused verbatim - this is a surface change)

| Field | On | Holds | Consumed by |
|-------|----|----|-------------|
| `UserAlignValue` | `DeclTypeAndValue` (LLVMBackend.h:662) | notions 1 & 2 (the `alignas` value) | struct layout; local alloca align via `CreateLocalVariable` (LLVMBackend.h:10304-10311) |
| `AllocAlignValue` | `TypeAndValue` base (LLVMBackend.h:492) | notion 3, DECLARED (the block a field/param/return promises it owns) | store-check, move-param call check; stamped onto a value on read |
| `AllocAlignment` | `NamedVariable` (LLVMBackend.h:690) | notion 3, ACTUAL (alignment of the concrete block a value owns now) | free site: `> kDefaultNewAlign` (16) routes `delete[]` to `__delete_aligned` (LLVMBackend.h:1597,1605) |

Flow today is OUTBOUND from `new`: `ParseNewExpression` computes the alignment and pushes it via
one-shot `lastAllocAlignment` (LLVMBackend.h:925), which the owning local picks up in
`ParseDeclaration` (MainListener.h:7108). Field source-of-truth already works the other way: a field
read stamps `namedVar.AllocAlignment = structField.AllocAlignValue` (LLVMBackend.h:13971).
Serialization round-trips `AllocAlignValue` under the `"aa"` key (LLVMBackend.cpp:3787/3830).

## The unified design

### Syntax: one keyword, two args, PREFIX (attribute) placement

```
alignmentSpecifier
    : 'alignas' '(' (typeName | constantExpression) (',' constantExpression)? ')' ;
```

- arg1 = SLOT / type alignment (today's `alignas`; `typeName` legal only in the 1-arg form).
- arg2 = ALLOCATION alignment of the owned heap block (today's `align_alloc`).
- `align_alloc` keyword and the declarator-SUFFIX placement are DELETED.

`alignas` is ALREADY a `declarationSpecifier` (CFlat.g4:262) parsed in `ParseDeclarationSpecifiers`
(MainListener.h:2658) - i.e. it already lives in the leading, C++-attribute position. So the unified
form lands where `alignas` already is; only `align_alloc`'s alloc-align moves from suffix to prefix:

```cflat
alignas(0, 64) double[] buf = new double[1024];   // slot natural, block 64-aligned
struct M { alignas(0, 64) double[] data = default; ~M() { delete[_] data; } };
```

`alignas(64)` and `alignas(SomeType)` stay valid (bare-slot short forms) -> full back-compat. `0` in
arg1 means "natural slot" (0 is already the unset sentinel for `UserAlignValue`).

### Placement semantics: prefix = whole-declaration distribution

A declaration-specifier applies to EVERY declarator in the statement, so `alignas(0,64) double[] a, b;`
gives BOTH `a` and `b` alloc-align 64. This is CONSISTENT with slot-align, which already distributes
that way via `UserAlignValue`. The only thing lost vs the old suffix is per-declarator DIFFERENT
alloc-align in one statement (`double[] a align_alloc(64), b;`) - rare; split the declaration if needed.

### Declaration is the authority; `new` INHERITS (inbound channel)

The declaration carries both alignments; a bare `new` on the RHS inherits the allocation alignment
from the target's `TypeAndValue`. Mirror of the existing outbound `lastAllocAlignment`:

1. Parse `alignas(0,64) double[] buf` -> `UserAlignValue=0`, `AllocAlignValue=64` on the local's TAV.
2. BEFORE evaluating the initializer, `ParseDeclaration` sets a new one-shot member
   `pendingInitAllocAlign = typeAndValue.AllocAlignValue`. (Parse order guarantees the declared type
   precedes the initializer - confirmed: `typeAndValue` is fully built by MainListener.h:7108.)
3. `ParseNewExpression`, when it has NO clause of its own, reads `pendingInitAllocAlign` to drive
   `allocAlign` -> routes to `operator new(size, 64)` + emits `CreateAlignmentAssumption`. Clears it.
4. Local's `AllocAlignment` set from the declaration (as today from `lastAllocAlignment`) -> scope-exit
   `delete[]` routes to `__delete_aligned(ptr, 64)`.

Same for direct assignment: `buf = new double[1024]` where `buf` was declared `alignas(0,64)` sets
`pendingInitAllocAlign` from `buf`'s `AllocAlignValue` before evaluating the RHS. An explicit
`alignas(_, N)` on the `new` remains allowed as an override / cross-check (error if it disagrees with
the target - reuse the existing store-check logic).

### The boundary that must be drawn (safety)

The inbound context must reach the `new` BEFORE it emits the allocation. Clean for the two DIRECT
shapes (decl-init, direct assignment to an align-declared target). NOT automatic when the `new` is
indirect - inside a ternary, or passed straight as a call argument to an `align_alloc`/`alignas(,N)`
param (`f(new double[1024])`). There the danger is a SILENT UNDER-ALIGNED allocation (allocate 16,
free as 64 -> corruption; unsound assume-aligned). Rule:

> Infer only for direct decl-init and direct assignment to an align-declared target. Anywhere the
> context cannot reach the `new`, REQUIRE an explicit `alignas(_, N)` on the `new`, and ERROR rather
> than silently under-align: "cannot infer allocation alignment here - annotate the `new`".

### Internal mapping (almost nothing new)

- arg1 -> `UserAlignValue` (existing path, untouched).
- arg2 -> `AllocAlignValue` (exactly what `align_alloc` fed).
- store-check, move-param call check, field-read stamp, `__delete_aligned` routing, `"aa"`
  serialization: all UNCHANGED (they already read `AllocAlignValue`).
- ONE new member: inbound one-shot `pendingInitAllocAlign` (symmetric to `lastAllocAlignment`).

## RETURN escape (the deferred piece, finished here)

Return was deferred in the `align_alloc` landing because it needs a call-RESULT channel: a function
whose return type is `alignas(_, N) T[]` must stamp the caller's receiving local's `AllocAlignment`
so the caller frees correctly. Add `lastCallReturnsAllocAlign` (mirror of the return-owned flags
already threaded through the call path), set from the callee's return-type `AllocAlignValue`, consumed
in `ParseDeclaration` where the other `lastCallReturns*` flags are consumed (MainListener.h:~7100).
With inbound inheritance also in place, `return new double[1024];` inside such a function inherits N
from the return type the same way an assignment does.

## Open decision: positional `alignas(0, 64)` vs named `alignas(alloc: 64)`

POSITIONAL (`alignas(0, 64)`) is what this plan specs. Two warts:
- The common case (a SIMD field that cares only about the BLOCK) is forced to write a dummy slot:
  `alignas(0, 64) double[] data` - the `0` is noise.
- On `new`, there is no slot, so arg1 would mean "allocation" there and "slot" on a declaration -
  a context-dependent first argument (the very ambiguity the two-keyword split removed). Mitigated by
  the inbound-inheritance design, since a bare `new` usually carries NO clause at all.

NAMED-OPTIONAL (`alignas(alloc: 64)` / `alignas(slot: 8, alloc: 64)`) removes both: alloc-only needs
no dummy, and on `new` only `alloc:` is legal (a `slot:` there is a clean error - no positional
meaning shift). `alignas(64)` / `alignas(T)` stay as bare-slot short forms. Same two internal sinks.
RECOMMENDATION: named-optional. Decide before implementing - it changes the grammar rule only.

## Implementation steps

1. Grammar (`CFlat.g4`): widen `alignmentSpecifier` (positional or named per the decision above);
   delete `allocAlignmentSpecifier` and its suffix uses on `initDeclarator` / `parameterDeclaration`
   / `newExpression`.
2. `ParseDeclarationSpecifiers` (BOTH copies - ForwardRefScanner ~ MainListener.h:970 and codegen
   ~MainListener.h:2658 - per the both-pass rule): read arg2 into `declType.AllocAlignValue`.
3. Add inbound one-shot `pendingInitAllocAlign` (LLVMBackend.h, next to `lastAllocAlignment`); set in
   `ParseDeclaration`/assignment before initializer eval; consume in `ParseNewExpression` when it has
   no explicit clause; ERROR on the indirect-context boundary.
4. RETURN: add `lastCallReturnsAllocAlign`, set from callee return-type `AllocAlignValue`, consume in
   `ParseDeclaration` alongside the other `lastCallReturns*` flags.
5. Remove the now-redundant `ParseAllocAlignmentSpecifier` helper; fold its validation (power-of-two,
   1..4096) into the arg2 path of `ParseAlignmentSpecifier`.
6. Migrate the landed tests: `Test/test_basic.cb` (`testAlignas` FlatMatrix/consumeFlat) and
   `Test/errors/err_align_alloc_mismatch.cb` from `align_alloc(...)` to the new spelling; add a
   RETURN positive case and an "cannot infer allocation alignment here" negative case.
7. Docs: `doc/LANGUAGE.md` alignment section.

## Verify (macOS arm64 host)

`./cmake_build.sh release`; `./test.sh Release` green; scratch `Matrix` repro compiles/runs and frees
via `__delete_aligned` (check IR + Guard Malloc); mismatch/under-align-inference cases error cleanly;
`--init` then recompile confirms the `"aa"` bitcode round-trip still holds.

## Out of scope

Containers (`list<T*>`): the element free is shared across every instantiation, so it would need the
alignment in the element TYPE - unreachable by any per-declaration clause, positional or named. A
`aligned<T, N>` WRAPPER TYPE (alignment back in the type identity, escapes for free, decays to `T[]`
for kernels) is the only design that reaches containers; noted as a separate future direction.
