Bucket: B (wrong value; C++ function-template argument spelling). Queue run B2 (parked 2026-09-28, restart from master on opus).

# String literal template arguments deduce const char* (C++: const char[N]); char* expressions unspellable

Found 2026-09-27 (fix/strlit-ref). Pre-existing. Oracle: scratch/b2p/b2_oracle.cpp (clang++ -std=c++20).

1. A literal passed to a template `U&`, `U&&` or `const U&` deduces `U = const char*` in CFlat, C++ deduces
   `const char[N]`: `nref("abc")` (returns sizeof(U)) gives 8 vs clang 4; `cnref("abc")` 8 vs 4;
   `fwdk("abc")` 2 vs 1. Cause: the template wrapper sends a literal as `const char *` first and only
   spells it inline on a failed call (RequestCxxFunctionTemplate argument loop, CxxStringLiteralSpelling /
   literalCallArguments).
2. `s + 1` (`ptr_first(spellPtr + 1)`, clang 98) and `cond ? "a" : "b"` as template arguments are refused:
   the argument is typed `(char)` / "cannot be spelled in C++".
3. On master a NAMED char array into `nref(U&)` is also refused (non-const lvalue-reference check,
   CInterop ~8472); B2 round 3 made it work (nref(a) 4, fwdk(a) 1) by exempting decayed arrays there.

B2 (parked, scratch/b2_parked.patch) did not fix 1 or 2 after three rounds; see
cpp-std-min-long-and-pointer-arguments-refused.md for why the expression-wide identity approach regressed.
Direction: for reference positions spell the literal as the array lvalue
`*reinterpret_cast<const char (*)[N]>(p)` with caller-frame storage (like the member decayed-array path,
e6c60220), decided BEFORE the `const char *` first attempt; spell a char* rvalue as `char*`.
Legs near 2761-2764 in Test/test_cpp_interop_template.cb.
