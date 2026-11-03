# Generic templates keep one slot per name: different signatures silently replace

- Different-signature templates with one name (`two(T)`/`two(T, T)`, `pv(T)`/`pv(T*)`) replace the earlier body silently: genericFunctionTemplates holds one slot per name.
- Test/errors/err_cpp_capturing_closure_to_cpp.cb:27/85 currently relies on that replacement (expects `two_generic<int>` to be `int(int, int)`).
- Duplicate generic member methods (`struct S { T f<T>(T a) ... T f<T>(T a) ... }`) are silent: MainListener_Aggregates.cpp:1498 (struct) and :5374 (class) register without an origin check.
- Maintainer question: do overloads become real overloads, or is a second template of the same name a redefinition error?
- Probes: scratch/repro_keep/natfix/natfix_rev/g1b.cb, g8.cb, g10.cb (main checkout, gitignored).
Also silent: nested generic args are not positional (`lc<T>(List<T>*)` vs `lc<U>(List<U>*)`), only a top-level type parameter maps to its index.
