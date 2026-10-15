# `move f()` of a lowered CFlat struct with a C++ member bit-copies (no move ctor runs)

Found 2026-10-15 while probing T258 (same family as the named/element move fixes T253/T258; pre-existing on master).
Repro (scratch/t258_tmp/cases/n18.body.cb, clang twin .body.cpp): `HV d = move mk();` and `HV[1] arr = { move mk() };`
with `HV mk()` returning a CFlat struct holding a C++ class `r253.C m`: CFlat 5; 1,0,0,1 vs clang 1005; 2,0,1,2.
The same bit copy happens for `W w = { h = move mk() }` (T258 named form, call-temp source).
Root cause (hypothesis, unverified): ParseMoveExpression has no named storage for an rvalue call result, so the
member-wise move (EmitCxxCopyOrMoveConstruct) that T253/T258 route through LoweredBraceMoveScope has no source slot.
Fix direction: materialize the call result into a temp slot and move-construct from it member-wise, at the decl,
positional-element and named-field sites (one helper); the temp must still be destroyed once.
