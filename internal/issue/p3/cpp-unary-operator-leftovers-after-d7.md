# Unary operator leftovers after D7+C3

Summary: selected unary follow-ups from D7+C3. This round fixes items 1, 3, 4, 7, 8a, 8b and 9. Remaining items are listed at the end.

## Fixed in this round

1. A free C++ `operator-(T&)` / `operator-(const T&)` pair now ranks `T&` for mutable lvalues and `const T&` for const lvalues and rvalues. The same `ComputeOverloadFunction` ranking applies to the matching binary free-operator pair.
3. A native CFlat member unary operator that mutates `this` now receives the lvalue slot itself. Pointer dereference mutates its pointee; temporaries stay temporary. Regression legs check value and exact destructor counts.
4. A free C++ unary operator taking non-const `T&` rejects a call-result temporary with the free-path rvalue-reference diagnostic. Lvalue and pointer-dereference acceptance are pinned.
7. Unary dereference of a C++ value with an `operator int*()` reports a `LogError` when the converted pointer cannot be lowered, instead of reaching module verification.
8a. Unary `!` operator lookup runs before the operator-bool shortcut, so a member `operator!` wins in `!nb` when both are present.
8b. A const-reference call result uses the `const T&` free unary overload and preserves the referent.
9. Unary minus for floating SIMD vectors emits floating negation. Unary `-`, `+` and `~` promote bool to signed int, matching C integer promotions.

## Still open

2. C++ conversion operators to unsigned or long long are not bound; unsigned promotion in the unary implicit-conversion step remains untested.

5. Unary conversion-step gaps remain: inherited conversions (including `std::atomic<int>`), reference-returning conversions, conversions to signed/unsigned char, long long, unsigned and unscoped enum, and pointer conversion `+cp`.

6. Pointer conversions are not counted as unary candidates. `IP { operator int() const; operator int*(); }` with `+ip` still needs to report ambiguity like clang.

8. Three C++ ranking cases remain open: free and member operators both declared (clang reports ambiguity); by-value versus `const T&` free pairs; and a ternary lvalue operand with a mutating operator running on a copy.

10. (b19 review 2, probes scratch/repro_keep/b19/rev2/) A refusal after the const/rvalue filter erases every candidate prints "Candidates (0):" (`wd.crefM() % 1` with only free `operator%(M&, int)`); it should name the dropped `M&` candidate and the const/rvalue reason. Same shape on the unary filter.
11. Pre-existing on master (b19 review 2): reversed free `one == c` with `operator==(const C&, int)` refused (clang accepts); a non-const member `operator+(int)` on a const global picked over a free `const&` (clang picks the free one); `mkV() + 1` with a by-value `V` free operator param refused as not addressable in one route; `inline const W<int> gW` not bound ("not a member").
