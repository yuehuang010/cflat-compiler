# Mixed-class C++ conditional residue after MIXCOND2

MIXCOND (237dbf3cc) landed decl / sret / single-argument sink; MIXCOND2 (0870063c4) landed the last argument of a
multi-argument CFlat, C++ or method call (earlier arguments converted as the regular call path converts them), a nested
conditional declaration arm, and the F1 blocker (a CFlat-produced prvalue arm now lives in the caller's full-expression
ledger). Both ABIs verified (MS: a reference-taking callee leaves B to the wrapper temporary; a by-value one destroys it
in the callee). Repros scratch/repro_keep/mixcond2/, clang++ -std=c++20 oracle.

## Still refused (clang accepts)
- Operand of a binary/unary/cast op: `take(k ? mkA(11) : mkB()) * 2` (c24, c37).
- Overloaded callee with one-argument candidates: `ov(k ? mkA(11) : mkB())` with two overloads (c07).
- Defaulted trailing parameter (S2); a conditional not in the last argument position; named arguments; an earlier
  argument that is a C++ class temporary or a long/double-to-narrower conversion.
- Return of a converting by-value parameter lvalue arm: `return k ? a : mkNB();` (S3, c25).

## Destructor order (values match clang)
The converted B dies when the callee returns inside the arm wrapper, not at the end of the caller's full expression:
`t2(t1(k ? cmkA(11) : mkB()), mkB())` gives 167574 vs clang 165774 (pre-existing on the single-argument path), and the
nested `t2(t2(1, k ? ...), k ? ...)` cell gives 167574 / 57167 vs clang 165774 / 516747.

## Fix direction
B in caller-owned storage registered on the full-expression ledger (like the producer A), so the wrapper only constructs
it; then the operator/overload refusals can route through the same callback.
