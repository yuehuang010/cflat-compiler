# Global `T g{}` and array `T[2] a = { {}, {} }` of an initializer_list-only C++ class refused

From T134 review 1 (2026-10-12), pre-existing on master c838c996e. T134 covers local `T v{}` / `T v = {}` and field
`= {}` / `v{}`; these two shapes still take the no-default-constructor refusal.

`struct L { int n; L(std::initializer_list<int> v) : n((int)v.size()) {} ~L() {} };`

- `r.L gl{};` at file scope -> "has no default constructor cflat can call - a global of it cannot be constructed".
- `r.L[2] a = { {}, {} };` -> "has no default constructor cflat can call, so an array of it cannot be default-initialized".

clang++ -std=c++20 accepts both (each element / the global gets the list ctor with an empty list, n == 0).
Fix direction: reuse the T134 empty-brace list-ctor route for global and array-element brace initializers.
