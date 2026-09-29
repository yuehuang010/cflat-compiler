# Unary operator leftovers after D7+C3

Summary: out-of-scope gaps found by the D7+C3 implementer (2026-09-29); probes are in cflat-fix-d7 scratch/d7/.
1. A const/non-const FREE overload pair for one class, `operator-(const T&)` beside `operator-(T&)`: an lvalue operand picks the first declared, while clang picks the non-const one.
   - The binary operators have the same gap; it is a ranking gap in ComputeOverloadFunction.
2. C++ conversion operators to unsigned or long long are not bound at all; `unsigned u = obj;` is refused on master.
   - As a result, unsigned promotion in ConvertUnaryOperandViaImplicitConversion is untested.
3. A native CFlat struct member `operator-()` applied to an lvalue still runs on a copy. D7 fixed this for C++ receivers only, so a mutating native unary operator leaves the source unchanged.

From the D7 review round 1 (scratch/repro_keep/d7r1/d7_review1.md, all non-regressions):
4. The free-path rvalue refusal never fires for call results. `~rw.mkFR2()` with `int operator~(FR2&)` gives 4, where clang says "invalid argument type". The sret temp passes the plain-load test.
5. Conversion-step gaps:
   - a conversion inherited from a base: DerI, and `-a` on std::atomic<int>;
   - reference-returning conversions: `operator int&()`, `operator const int&() const`;
   - targets signed/unsigned char, long long, unsigned, and unscoped enum;
   - pointer conversion `+cp`.
6. Pointer conversions are not counted as candidates. `struct IP { operator int() const; operator int*(); }` with `+ip`: clang reports ambiguity, cflat gives 3.
7. `*cp` through `operator int*()` fails with "Module verification failed". Pre-existing verifier trap.
8. C++ ranking mismatches:
   - `!nb` with both a member operator! and operator bool gives 0; clang 77. The operator-bool shortcut runs before operator lookup.
   - `-rv.crefCN()` on a const& result picks the non-const overload.
   - free and member operators both declared: cflat picks the free one; clang reports ambiguity.
   - by-value vs const& free pair: cflat picks const&.
   - a ternary lvalue operand `-(f ? m : n)` with a mutating operator runs on a copy.
9. Native:
   - `-s` on simd<float,4> fails verification ("Integer arithmetic operators only work with integral types").
   - `-bb` on bool gives 1; C gives -1.
Const receiver overload pick and const fields: see p2/cpp-const-object-nonconst-member-call-writes-readonly.md.
