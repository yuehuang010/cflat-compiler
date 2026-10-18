# is_assignable: grouped operand and namespace-alias / relative generic spellings

Summary: `is_assignable(X)` (const phase 2, R5a) still misreads two operand shapes (Sol review c02 r4):
1. Grouped operand `is_assignable((C))` with `struct C { const int n = 0; }` returns true (clang
   std::is_copy_assignable_v<C> = 0). Grouped aliases, `(ns.G<C>)` and a grouped substituted T do the same; warm cache too.
2. `using n = ns; is_assignable(n.G<C>)` and, inside `namespace outer`, `is_assignable(ns.G<C>)` for outer.ns.G fail
   with "cannot find the type 'ns.G<C>'" unless the type is used elsewhere first.

Fail-safe today: a wrong true only means the guarded assigning body compiles and the whole-assign #13 refusal fires at
instantiation instead of the compile_error text. Nothing becomes writable.

Repros: scratch/repro_keep/const_c02/rev4/fallback_paren.cb, ns_alias.cb, ns_relative.cb (+ .cpp oracles).

Root cause: (1) grouping blocks substitution and typeName parsing; the fallback passes "(C)" to FindNativeConstField
(cflat/MainListener_PostfixExpression.cpp is_assignable block), whose missing key becomes true. (2)
ForwardRefScanner.cpp ScanGenericTypeUses expression case (and the typeSpecifier gate next to it) tests the raw spelling
with IsGenericTemplateKey before alias / current-namespace resolution.

Fix direction: unwrap grouping first; reject an operand that does not resolve as a type instead of answering true;
resolve the template base with ResolveGenericTemplateBase before the key test in both scanner gates.
