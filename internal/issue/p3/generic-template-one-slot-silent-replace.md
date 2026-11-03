# Generic templates keep one slot per name: different signatures silently replace

- Different-signature templates with one name (`two(T)`/`two(T, T)`, `pv(T)`/`pv(T*)`) replace the earlier body silently: genericFunctionTemplates holds one slot per name.
- Test/errors/err_cpp_capturing_closure_to_cpp.cb:27/85 currently relies on that replacement (expects `two_generic<int>` to be `int(int, int)`).
- Duplicate generic member methods (`struct S { T f<T>(T a) ... T f<T>(T a) ... }`) are silent: MainListener_Aggregates.cpp:1498 (struct) and :5374 (class) register without an origin check.
- Maintainer question: do overloads become real overloads, or is a second template of the same name a redefinition error?
- Probes: scratch/repro_keep/natfix/natfix_rev/g1b.cb, g8.cb, g10.cb (main checkout, gitignored).
Also silent: nested generic args are not positional (`lc<T>(List<T>*)` vs `lc<U>(List<U>*)`), only a top-level type parameter maps to its index.
RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): generic templates OVERLOAD like plain functions - C++ function
templates and C# generic methods both overload by arity/parameter shape, and CFlat already keeps an overload registry for
non-generic functions (functionTable). Until the per-name slot becomes a set, a second template of one name is a REDEFINITION
ERROR, never a silent replacement; a same-signature duplicate (free or member) is a redefinition error always. Nested generic args
become positional with the same change. Test consequence: Test/errors/err_cpp_capturing_closure_to_cpp.cb:27/85 is rewritten with
distinct template names (it pinned the replacement); one new err_ leg for the duplicate; `two(T)` + `two(T, T)` becomes an accept leg.
