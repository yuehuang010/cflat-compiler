# `returnsOwned` for operator+ / operator string(i32) drifts between the two ParseDeclarationSpecifiers copies

Summary: the `returnsOwned` computation is INCONSISTENT between the two function-
declaration paths in MainListener.h.

- ForwardRefScanner copy (~L811-820): special-cases `operator+` and
  `operator string(i32)` to `returnsOwned = true` in addition to the generic
  `returnType.IsMove` check.
- Definition copy (~L4878-4887): only checks `IsMove` (plus the pointer /
  interface move cases). It does NOT special-case `operator+` /
  `operator string(i32)`.

Harmless today: the forward-decl copy is what populates the function-table
`ReturnsOwned` used at CALL sites (so operator+ results are still registered as
owned temps and freed - see Test/test_collection_leaks.cb "plain-fn temp '+'
operands"). The definition copy's `returnsOwned` feeds `currentFunctionReturnsOwned`
during body codegen and the table `ReturnsOwned` on the definition entry, but
operator+ / operator string return values are call results (not loads from a
local), so `currentFunctionReturnsOwned` does not change their codegen. It is a
latent two-copy drift that would bite if that flag is ever consulted more
broadly.

## Why deferred (2026-07-03)

Aligning the definition copy means computing `returnsOwned = true` for
`operator+` and `operator string(i32)` when their DEFINITIONS (in core/string.cb)
are processed. That sets `currentFunctionReturnsOwned = true` during those bodies'
codegen (createFunctionBlock, LLVMBackend.h ~L1793) and flips the definition-entry
table `ReturnsOwned` (CreateFunctionDefinition, ~L11338). Both feed the hottest
owned-string path (every string concat). Changing return-value handling on that
path for a documented-harmless drift is a poor risk/reward trade during a scoped
leak fix, so it is deferred rather than folded in.

## Fix direction (when revived)

Extract the `returnsOwned` computation into ONE shared helper called by both
`ParseDeclarationSpecifiers` copies (the CLAUDE.md rule that any type-parsing
change must be applied in both copies is exactly what drifted here). Then verify
core string operator codegen is byte-identical (compare `--out-lli` of a concat
before/after) and that test.bat + test_collection_leaks.cb stay green.
