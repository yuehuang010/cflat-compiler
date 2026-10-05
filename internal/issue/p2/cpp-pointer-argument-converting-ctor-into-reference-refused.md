# A pointer argument for a C++ reference parameter is refused where clang accepts a converting ctor

FIXED 2026-10-02 (T11): a pointer argument no longer binds a reference candidate whose referent differs from the
pointee (`twoRef(int&)` + `twoRef(double&)` with `int*` used to bind `double&` and read int bytes as double; a
named `int*` is now refused like the lone `f(int&)` case, `&x` binds `int&`).
What remains is the refusal below.

## Remaining (refusal)

A pointer argument to a C++ `X&&` / `const X&` parameter that clang accepts via a converting constructor from the
pointer (`strTwo(const std::string&)` + `strTwo(std::string&&)` with `char*`, `vector<string>.push_back(mp)`,
`map<string,int>[mp]`) is refused with an empty source type ("from ''"). Ranking now picks clang's candidate;
lowering a pointer through a converting ctor into an rvalue-reference parameter is missing.
