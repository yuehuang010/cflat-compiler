# P3: C++ overload selection gaps left after W13 (defaulted/member variadics, user conversions)

Summary: W13 (2026-09-30) made const record pointers select C++ overloads like clang (a candidate that drops pointee const is not viable; free/member functions, operators, variadic siblings, templates, constructors) and ranks a C++ variadic whose ellipsis receives no argument on its declared params. The gaps below are the same on master, measured against `clang++ -std=c++20`. Probes: scratch/repro_keep/w13/ (rev15 = s3.h + oracle.cpp + run15.sh; rev17 = v.h/v.cpp variadic set + run.sh; rebuild libv.a from v.cpp).

(Item 1, ambiguity in overload sets with a variadic, is fixed: an all-C++ set with a variadic
whose ellipsis receives arguments now runs the pairwise [over.match.best] ranking, an ellipsis
argument ranking worst. Items keep their original numbers.)
4. User-defined conversions are never candidates at C++ call arguments: `u1(P*)`, `u1(D*)`, `u1(CA)` with `CA(const P*)` and a `const P*` argument -> clang 3, CFlat refuses; `u3(cp)` with only `u3(CA)` refuses.
5. Inline C++ variadic definitions in a header fail to link (vf, vmv, vom in rev15).

Ruling (maintainer, 2026-09-30): C++ call arguments keep CFlat's stricter conversion rule (no int ->
double etc. at calls), for type safety - NOT clang's arithmetic conversions. `n1(P*, int)` /
`n1(const P*, double)` with `n1(cp, 5)` stays an error naming the candidate; the workaround is
`n1(cp, 5.0)` or `n1(cp, (double)n)` (verified). Item 4 (user-defined conversions) is unaffected:
it is about class conversions, not arithmetic ones.

## Item 1 leftovers (Q7 Sol r1, 2026-10-01; master-identical)
- Constructor sets: `C(int, ...)` / `C(long, int)`, `C(1, 2)` - clang ambiguous, CFlat runs C(long, int). Ctor projection omits the variadic (LLVMBackend_CInterop.cpp ~12405, ~18655); CxxConstructorNeedsClangResolution is false without ctor templates.
- C-variadic function templates skipped by CClangExtract.cpp ~1401 (oracle 1, CFlat 2).
Repros: scratch/repro_keep/q7_followups/{zero_min,zdef,ctor_min,template_min}.cb with .cpp oracles.
