# P3: C++ overload selection gaps left after W13 (variadic sets, user conversions)

Summary: W13 (2026-09-30) made const record pointers select C++ overloads like clang (a candidate that drops pointee const is not viable; free/member functions, operators, variadic siblings, templates, constructors) and ranks a C++ variadic whose ellipsis receives no argument on its declared params. The gaps below are the same on master, measured against `clang++ -std=c++20`. Probes: scratch/repro_keep/w13/ (rev15 = s3.h + oracle.cpp + run15.sh; rev17 = v.h/v.cpp variadic set + run.sh; rebuild libv.a from v.cpp).

1. Ambiguity is never detected for a C++ overload set containing a variadic: `b1(int, ...)` / `b1(long, int)` called `b1(1, 2)`, `c1`, `e1m`, `vom` - clang reports ambiguous, CFlat silently picks the non-variadic sibling (master picked the variadic). Run the C++ pairwise ranking over sets with a variadic.
2. A defaulted param before the ellipsis loses: `f1(int, int = 0, ...)` vs `f1(long)`, `f1(1)` -> clang 1, CFlat 2 (omitted-default penalty).
3. Member variadics: `m(int, ...)` vs `m(long)`, `m(1)` -> clang 1, CFlat 2 (the W13 empty-ellipsis rule does not reach the member path); `m(1, 2)` (argument into the member ellipsis) fails with "no overload matches".
4. User-defined conversions are never candidates at C++ call arguments: `u1(P*)`, `u1(D*)`, `u1(CA)` with `CA(const P*)` and a `const P*` argument -> clang 3, CFlat refuses; `u3(cp)` with only `u3(CA)` refuses.
5. Inline C++ variadic definitions in a header fail to link (vf, vmv, vom in rev15).
6. Non-record const pointers carry no pointee const: `ci(int*)` / `ci(const int*)` with a `const int*` picks 1 (clang 2); same for `const char*` (cc/ccc, cic).

Ruling (maintainer, 2026-09-30): C++ call arguments keep CFlat's stricter conversion rule (no int ->
double etc. at calls), for type safety - NOT clang's arithmetic conversions. `n1(P*, int)` /
`n1(const P*, double)` with `n1(cp, 5)` stays an error naming the candidate; the workaround is
`n1(cp, 5.0)` or `n1(cp, (double)n)` (verified). Item 4 (user-defined conversions) is unaffected:
it is about class conversions, not arithmetic ones. Item 1 (ambiguity in variadic sets) awaits a
ruling after the splash measurement.
