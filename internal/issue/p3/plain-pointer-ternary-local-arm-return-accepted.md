# A returned `?:` of plain field addresses with a frame-local arm is accepted

Bucket: p3 (dangling acceptance, CFlat-native, pre-existing). Found 2026-09-24 by the CFlat alias
ternary return fix, which deliberately left it alone.

## Summary

`Row* f(Tab* t, bool c) { Tab local; return c ? &local.r : &t->r; }` compiles; `return &local.r;`
alone is refused as returning the address of a local. The join loses the frame-local provenance
of one arm, the same shape the alias-result ternary had before its fix.

## Fix direction

Apply the direct form's address-of-local check per arm when the returned value is a PHI/select
join (see the CFlat alias join walk in EmitReturnExpression, cflat/MainListener_Statements.cpp).
Measure `--check` over Test/ and example/ for new refusals first.
