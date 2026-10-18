# C++ brace-argument overload ties clang calls ambiguous: leftovers after T314

T314 (2026-10-16) refuses a single braced argument whose overloads tie exactly on a fully modelled arithmetic rank
(cflat/LLVMBackend_CInterop.cpp RequestCxxBraceFunction, braceElementRank + `certain`). Every shape below is still
silently resolved (master identical; T314 reviews 1-2), clang++ -std=c++20 result in brackets:
- two braced arguments that tie: `pb2({1,2},{3,4})` with (il<long long>, il<long long>) / (il<double>, il<double>) [ambiguous]
- a plain argument beside the brace that ties too: `pa(1, {1,2})`, member `m.mq(1, {1,2})` [ambiguous]
- default-argument extra parameter: `da(il<long>, int = 0)` vs `da(il<double>)` with `{1,2}` [ambiguous]
- `fl(il<float>)` vs `fl(il<long double>)` with `{1}` [ambiguous]
- template candidate `tp(il<T>)` vs `tp(il<long>)` with `{1,2}`: CFlat 2 [clang 1]
- const member on a non-const object, `m(il<long>) const` vs `m(il<double>)`: CFlat 1 [clang 2]; `const&` vs `&`,
  `volatile` vs unqualified receivers: CFlat 1 [clang 2]; both `&` or both `volatile` tie: CFlat 1 [ambiguous]
- `pd({1,2}, 1)`: CFlat 1 [clang 2]
Cosmetic: the ambiguity candidate list spells `std::initializer_list<...>` and unqualified member names (`'m'`).

## Fix direction

Each needs its rank modelled before it may refuse (the `certain` gate stays conservative); per-argument dominance for
several braced/plain arguments, default-argument viability, template-vs-non-template tiebreak, receiver cv/ref ranking.
