# Generic function template overload sets remain unsupported

- Fixed: same-name generic free-function and member-template definitions cannot silently replace the registry slot. Same-shape duplicates report a redefinition and first-definition location; a different shape reports `generic template overloading is not supported yet` with the first location.
- Remaining: `genericFunctionTemplates` still stores one function context per name. Implement per-name overload sets, deduction across candidates, ranking/specialization, explicit type-argument arity selection, and cache round-trip changes before accepting `two(T)`/`two(T,T)` or `pv(T)`/`pv(T*)`.
- Nested type arguments are positional for duplicate detection (`DupList<T>` and `DupList<U>` are the same shape).
- Repro coverage: `scratch/repro_keep/natfix/natfix_rev/g1b.cb`, `g8.cb`, and `g10.cb` in the main checkout; new regression legs are in `Test/errors/err_generic_template_redefinition.cb`.

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): generic templates OVERLOAD like plain functions - C++ function
templates and C# generic methods both overload by arity/parameter shape, and CFlat already keeps an overload registry for
non-generic functions (functionTable). Until the per-name slot becomes a set, a second template of one name is a REDEFINITION
ERROR, never a silent replacement; a same-signature duplicate (free or member) is a redefinition error always. Remaining test
consequence once overload sets land: `two(T)` + `two(T, T)` becomes an accept leg (the different-shape leg in
Test/errors/err_generic_template_redefinition.cb flips).
Review nits (gentpl r1, non-blocking): a shape key built from the type part only gives "redefinition" wording for templates that differ
only in a declarator (both refused); the generic-struct member wording prints 'Box<int.c>' instead of 'Box<int>.c'.
