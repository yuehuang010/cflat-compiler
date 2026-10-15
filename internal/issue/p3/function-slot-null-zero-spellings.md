# Null spellings into function slots refused where clang / direct calls accept

Pre-existing, all same on master (T185 Opus r1 + r2, 2026-10-14). Repros: scratch/repro_keep/t185_rv1/ (a21, r14, a17, a16), scratch/repro_keep/t185_rv2/.
- C `NULL` macro into function<> / C function-pointer slot / C struct fn-pointer field: "cannot assign a 'void*' value to
  'function<>'"; clang accepts (NULL is a null pointer constant).
- `k ? inc : 0` into function<> refused ("incompatible types 'pointer' and 'i8'"); `k ? p : nullptr` with a Lambda variable too.
- Literal 0 into a function<> parameter through an interface call `i.run(0, 0)` -> "no method matches" (direct call accepts).
- Literal 0 into a generic T = function<> parameter (`isNull<function<int(int)>>(0)`, `Box<function<..>>.set(0)`) ->
  "no overload matches"; nullptr and named functions work.
Fix direction: treat a null pointer constant (literal 0, NULL, nullptr) as convertible to any function slot at every site,
including ternary arm join and generic / interface argument matching.
