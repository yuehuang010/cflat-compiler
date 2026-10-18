# C++ binary operator: base free candidates with prvalue receivers

T307 resolved the nearer-base free ranking, equal-distance ambiguity, and inherited-member cv ranking recorded in the previous versions of this issue. T321 resolved the prvalue receiver cases (rvalue-reference and by-value base frees).

## Remaining T307 operator-ranking mismatches

The following independent forms were verified by the round-1 review against the master executable and clang. They remain open; round 2 does not alter their candidate construction or rewritten/compound operator paths.

- Reversed `==`: `1 == a` (`o3_eq_rev`) returns 1 on CFlat and 0 on clang; `a == 1` (`o3b_eq_fwd`) returns 0 on both.
- Rewritten `<=>`: `a < 1` (`o5_ss_fwd`) returns 1 on CFlat and 0 on clang; `1 < a` (`o5b_ss_rev`) returns 0 on CFlat and 1 on clang; the base-tie case (`o5c_ss_tie`) is selected on CFlat but ambiguous on clang.
- Compound `+=`: inherited member vs nearer free (`c1_pluseq`) selects the member on CFlat and free on clang; equal-distance (`c3_pluseq_tie`) selects on CFlat and is ambiguous on clang.
- Private-base free candidate (`m3_private`): CFlat selects it; clang reports ambiguity.
