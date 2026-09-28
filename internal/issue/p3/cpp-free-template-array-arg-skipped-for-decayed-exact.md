# Free function: array argument picks a non-template overload by CFlat decay instead of clang ranking

Found 2026-09-27 (AQ round-2 review). `fchar(char[4])` gives 9071 (clang 9078); `farr(int[2][2])`
gives 9061 (clang 9068), where a free function template taking the array by reference sits beside a
non-template pointer overload.

## Root cause

The free-function path skips the template request when CFlat's own array-to-pointer decay finds an
"exact" non-template, so clang never ranks the set. The member path (fixed in fix/mt-nontpl, e6c60220)
passes the real array lvalue to the wrapper via `*reinterpret_cast<E (*)[N]...>(p)`.

## Fix direction

Mirror the member path: when a same-named free template exists and the argument is an array, ask clang
with the array lvalue instead of short-circuiting on the decayed exact match. Legs beside the member
legs in Test/test_cpp_interop.cb (7927-7931).
