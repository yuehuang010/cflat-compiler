# C++ binary operator: base-class frees vs an inherited member (ranking not yet done)

Filed by T291 round 2 (2026-10-16). The reversed `<=>` relation part of T291 is fixed (round 1, d63680def). The
ranking work below is not: the round-1 attempt was backed out for a regression.

## 1. Intermediate-base free loses to the inherited member (clang 2, CFlat 1)

```cpp
struct B { int operator+(int) { return 1; } }; struct C : B {}; struct A : C {};
inline int operator+(C&, int) { return 2; }   // A a = default; return a + 1;
```

clang++ -std=c++20 picks the free over the nearer base `C&` (distance 1 from `A`) over the member inherited from `B`
(distance 2). CFlat (master and current tree) picks the member. Fix direction: when a member is inherited from `B` and a
free takes `C&` with `C` between the receiver and `B`, rank the nearer class first ([over.ics.rank] 4.4), in the
pinned member/free block (`MainListener_Expressions.cpp`, binary `TryBinaryOperatorOverload`) and the comparator
(`LLVMBackend_Overloads.cpp`, `CompareCxxConversionRanks`, the implicit-object branch).

## 2. Equal-distance tie (clang ambiguous, CFlat member)

```cpp
struct MB { int operator+(int) { return 1; } }; struct MC : MB {}; struct MA : MC {};
inline int operator+(MB&, int) { return 2; }   // MA ma = default; return ma + 1;
```

clang++ -std=c++20 reports the call ambiguous (member and free both bind `MB` at distance 2). CFlat master silently
selects the member. Target: refuse with the existing "ambiguous C++ binary operator 'operator+'" text, as T280 does for
a same-class tie. Probe: scratch/t291/inter2/tie.cb.

## 3. rvalue-reference free over a base, prvalue receiver (clang 2, CFlat 1)

```cpp
struct RB { int operator+(int) { return 1; } }; struct RC : RB {}; struct RA : RC {};
inline int operator+(RC&&, int) { return 2; }   // RA raMk(); return raMk() + 1;
```

CFlat ranks the `RC&&` binding of an `RA` prvalue as a user-defined conversion (rank 3, no source or target class;
`LLVMBackend_Overloads.cpp` converting-constructor path, near `out.rank = 3`), not as a derived-to-base binding.
Same-class `S&&` against a member is ambiguous in both clang and CFlat, which is correct. Fix direction: rank a
derived-to-base rvalue-reference binding as rank 2 with `toClass` set, then let it compete with the member.

## 4. By-value base free, prvalue receiver (clang 2, CFlat 1)

```cpp
struct VB { int operator+(int) { return 1; } }; struct VC : VB {}; struct VA : VC {};
inline int operator+(VC, int) { return 2; }    // VA vaMk(); return vaMk() + 1;
```

CFlat is silent and wrong. Without a member, `sl3Mk() + 1` on a derived prvalue passed to a by-value base parameter is
refused ("cannot cast an aggregate value"): a prvalue is not sliced into a by-value base. The lvalue form (`VA a; a + 1`)
is correct (2). Fix direction: slice a prvalue into the by-value base (copy the base subobject), then admit by-value
base frees on prvalue receivers.

## 5. Round-1 attempt, backed out: same-base cv qualification ignored

Round 1 (commit d63680def) added base-typed frees to the pinned block whenever an inherited member was present, and a
comparator rule for an inherited member object against a derived-to-base binding using `tieBreak` (4.4) on `toClass`.
That made item 1 pass but regressed legal calls. Sol's review (scratch/briefs/t291_review1.md) found:

- The inherited member's object qualification is stored in `object.second` (no `cvBase`), so `tieBreak` cannot compare
  it with the free's `reference.cv`. When both `toClass` values are the same base, `tieBreak` returns 0 and the branch
  returns 2 (incomparable). Both the member and the free are then winners, and the call is refused as ambiguous.
- Repros (scratch/rev_t291_sol_tmp/, each `samebase_<n>.cb` with `samebase_<n>.h` and `.cpp` as the clang oracle):
  - `samebase_mut`: `B::operator+(int)` (non-const) and `operator+(const B&, int)`, `D : C : B`. clang 1 (member),
    master 1, round-1 fix: refused as ambiguous.
  - `samebase_const`: `B::operator+(int) const` and `operator+(B&, int)`. clang 2 (free), master 1, round-1 fix:
    refused as ambiguous.
  - `samebase_constref`: `B::operator+(int) const &` and `operator+(B&, int)`. clang 2, master 1, round-1 fix: refused.
  - `samebase_volatile`: `B::operator+(int) volatile` and `operator+(B&, int)`. clang 2, master 1, round-1 fix: refused.

Master is already wrong on the three `const`/`volatile` cases (it picks the member, clang picks the free). The backed-out
change turned those into refusals and turned the `mut` case from correct into a refusal. Fix direction for this work:
compare the implicit object's cv and ref-qualifier against the free's reference cv for the same declaring base before
deciding they are indistinguishable. Keep the ambiguity only for bindings that are genuinely equal. Extend the existing
related tests with both cv directions (the `mut` leg must keep selecting the member; the `const`/`constref`/`volatile`
legs must select the free).

## Fixed by T291 round 1 (not repeated here)

- Reversed relations (`x @ y` as `0 @ (y <=> x)`) read the mirrored range when the ordering came from the reversed
  candidate. Probe: scratch/t291/rel (504 cases, all match clang) and scratch/t291/po (partial_ordering).
