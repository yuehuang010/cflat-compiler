# Function-pointer signature components carry a spelling, not a declaring scope

Filed 2026-09-03 while landing `centralize-scoped-registry-resolution` (q02 member 4).

## Summary

A function-pointer signature records each struct component by its SOURCE SPELLING (`ShadowPt*`)
without the namespace it was declared in. Component comparison therefore cannot use the normal
scoped resolver: the namespace active at comparison time is not the one the spelling was written
in. `FuncPtrStructCandidates` (`cflat/LLVMBackend_Lookup.cpp`) keeps the Q15 set semantics through
the named `TailMatchCandidates` primitive and uses recorded resolved keys to narrow ambiguity.

## Evidence

The scoped-first variant (resolve the spelling in the current namespace, tail scan only as
fallback) was built and measured: it false-rejects `Test/test_function_ptr.cb`'s
"namespace-ambiguous pointee binds" leg (`SigNs.runNs(gns)`) and `test_basic.cb`
`ns_type_funcptr_pointee_visible`. Both legs guard against re-trying it.

## Fix direction

Record the declaring scope (the fully scoped key the spelling resolved to at declaration) on the
signature component, and compare resolved keys. Then `TailMatchCandidates` can go, and the
recorded-key narrowing becomes the only path. Touches signature construction in both
`ParseDeclarationSpecifiers` copies if the component list is built there - check first.
