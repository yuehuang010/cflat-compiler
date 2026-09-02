# The builtin-`unique` remnant decision is ~280 lines of parse-tree lookahead with a third copy in the scanner

Filed 2026-09-02 from the Fable advisor verdict on the unique<T> branch (internal/plan/
unique-ownership.md, "FABLE ADVISOR VERDICT"). Maintainer ruling 2026-09-02: filed as an issue,
not a merge blocker; the branch merges with the remnant in place.

## What is wrong

`unique T*` on a LOCAL desugars to `unique<T>` in ParseDeclarationSpecifiers, then
MainListener::ParseDeclaration UNDOES the desugar ("preservesBuiltinUnique",
MainListener_Declarations.cpp ~3362-3644, applied ~3699) when the local must keep the legacy
builtin pointer representation. That decision is made by walking the parse tree by name and token
index:

- `hasRawNewArrayValue` (~3371): the initializer is a `new T[n]` (counted destruction needs the
  per-local length slot the builtin path owns).
- `hasAlignedMoveSource` (~3420): the initializer moves from an `alignas`-allocated local.
- `hasLaterBuiltinUniqueAssignment` (~3542): some LATER `name = new T[n]` / aligned move in the
  enclosing function assigns the local, scanning past redeclarations and for-declarations.

ForwardRefScanner::ParseDeclarationSpecifiers (ForwardRefScanner.cpp ~355-395) carries a second
copy of the desugar gate keyed on `IsLocalDeclarationSpecifiers` and the folded `alignas`; the
scanner never sees the initializer, so the two passes only agree by construction.

It is a second, syntactic ownership analysis that runs AHEAD of the real one and does not share
its provenance flags (`AllocatedByRawNewArray`, `RawArrayLength*`, `AllocAlignValue`). Rounds 2
and 3 of the Opus review each found a shape it misses (a `new` buried in a condition or call
argument, alignas folding divergence, the by-value consume gate). Test/test_core.cb
`runRawCount*` pins the shapes it currently handles.

## Fix direction (advisor recommendation, REPLACE)

(a) `new T[n]` into a unique LOCAL becomes a hard error like the field, parameter and return legs
    already are (Test/errors/err_unique_array_view.cb) - drops `hasRawNewArrayValue` and
    `hasLaterBuiltinUniqueAssignment`. The `runRawCount*` legs in test_core.cb move to a raw
    `T*` local (which already carries the count) or a container.
(b) builtin stays ONLY when the DECLARED type carries `alignas(_, N>0)`; both passes already fold
    that identically with FoldCompileTimeInt - drops `hasAlignedMoveSource` (an aligned pointer can
    only move into a declared-aligned slot).

About 200 of the 280 lines go and the decision becomes a pure function of the declaration
specifiers, shared by both passes. If (a) must stay legal, the minimum is: decide from the direct
initializer plus the declared alignas only, and LogError the later-assignment forms.

Related: internal/issue/p2/unique-field-heap-array-through-move-param.md (the count is lost at
every `move T*` boundary, not only here).
