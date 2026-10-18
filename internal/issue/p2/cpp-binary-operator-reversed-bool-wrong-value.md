# C++ binary operator: base free candidates with prvalue receivers

T307 resolved the nearer-base free ranking, equal-distance ambiguity, and inherited-member cv ranking recorded in the previous versions of this issue. The two prvalue cases below remain: they use separate conversion and value-lowering paths.

## 1. Rvalue-reference free over a base, prvalue receiver (clang 2, CFlat 1)

```cpp
struct RB { int operator+(int) { return 1; } }; struct RC : RB {}; struct RA : RC {};
inline int operator+(RC&&, int) { return 2; }   // RA raMk(); return raMk() + 1;
```

CFlat ranks the `RC&&` binding of an `RA` prvalue as a user-defined conversion (rank 3, no source or target class; `LLVMBackend_Overloads.cpp` converting-constructor path, near `out.rank = 3`), not as a derived-to-base binding. Same-class `S&&` against a member is ambiguous in both clang and CFlat, which is correct. Fix direction: rank a derived-to-base rvalue-reference binding as rank 2 with `toClass` set, then let it compete with the member.

T307 re-probe: clang++ -std=c++20 returns 2; the master and worktree Release compilers return 1 (`scratch/t307_matrix/out3.cb`).

## 2. By-value base free, prvalue receiver (clang 2, CFlat 1)

```cpp
struct VB { int operator+(int) { return 1; } }; struct VC : VB {}; struct VA : VC {};
inline int operator+(VC, int) { return 2; }   // VA vaMk(); return vaMk() + 1;
```

CFlat is silent and wrong. Without a member, `sl3Mk() + 1` on a derived prvalue passed to a by-value base parameter is refused (`cannot cast an aggregate value`): a prvalue is not sliced into a by-value base. The lvalue form (`VA a; a + 1`) is correct (2). Fix direction: slice a prvalue into the by-value base (copy the base subobject), then admit by-value base frees on prvalue receivers.

T307 re-probe: clang++ -std=c++20 returns 2; the master and worktree Release compilers return 1 (`scratch/t307_matrix/out4.cb`).

## Remaining T307 operator-ranking mismatches

The following independent forms were verified by the round-1 review against the master executable and clang. They remain open; round 2 does not alter their candidate construction or rewritten/compound operator paths.

- Reversed `==`: `1 == a` (`o3_eq_rev`) returns 1 on CFlat and 0 on clang; `a == 1` (`o3b_eq_fwd`) returns 0 on both.
- Rewritten `<=>`: `a < 1` (`o5_ss_fwd`) returns 1 on CFlat and 0 on clang; `1 < a` (`o5b_ss_rev`) returns 0 on CFlat and 1 on clang; the base-tie case (`o5c_ss_tie`) is selected on CFlat but ambiguous on clang.
- Compound `+=`: inherited member vs nearer free (`c1_pluseq`) selects the member on CFlat and free on clang; equal-distance (`c3_pluseq_tie`) selects on CFlat and is ambiguous on clang.
- Private-base free candidate (`m3_private`): CFlat selects it; clang reports ambiguity.
