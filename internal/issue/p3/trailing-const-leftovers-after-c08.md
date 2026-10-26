# Trailing const leftovers after c08 (const plan phase 8)

Filed 2026-10-19 from the c08 implementer report (landed 87b36b2f1). Items 1-3 (#8 field naming,
#10 location for generic interfaces, receiver on internal interface dispatch) fixed in c11.

Design note (not a bug): `T Read() const { return v; }` with T = int* is refused because const is transitive through
pointer fields (phase 2); C++ is shallow. Keep unless the maintainer rules otherwise.

## Left after T471 (Opus r2; master same unless noted)
- Namespace-scope variable named like a method still wins: `namespace n { Lambda<int(int)> gf; struct S { int gf(int); int go() { return gf(5); } } }`
  returns 25 (clang 15). PostfixExpression ~4189 `GetGlobalVariableNV(idName)` misses the namespace-qualified global; use the
  namespace-aware lookup ParsePrimaryExpression uses. Repro scratch/repro_keep/box1022/t471/d02_ns_var.cb (d06, d08 beside it).
- Static-member-hides-free inside a LAMBDA in a generic method: 25 (clang 15; non-generic lambda correct). EnclosingAggregateScopesForCall
  (MainListener.h ~2513) takes the lambda's own symbol from GetCurrentFunctionSymbol(); walk out to the enclosing method. Repro d06_lambda_in_generic.cb.
- Nested struct calling an outer instance method bare is refused (correct, clang too; master returned 25) but with the generic
  "no overload of 'm'" / `[0] i8 <this>` label; the specific c17 text requires implicitReceiver == nullptr (Overloads ~6393). Repro d08_nested_outer_method.cb.
- LSP: hover/definition trust the resolved-call map by position only (LspServer.cpp ~909/~1252); with a cached index kept for a file
  with errors, stale positions can answer another word. Guard: accept only when the target's short name equals the word under the cursor.
  RegisterResolvedCall is a linear search per call (O(n^2) on large files); key by (file, line, column).
