# Generic overload vs concrete overloads: needs-ruling picks and leftovers after GENOVL

GENOVL (ef83ffa02) lets a generic candidate join a native overload set with concrete overloads when no concrete candidate
resolves the call on its own, so every call master REFUSED and clang accepts now picks as clang (pointer, value, pair,
namespace, both declaration orders). Matrix and probes: scratch/repro_keep/genovl/ (genovl_report.md, genovl_review1.md,
review1/cases/).

## RULED 2026-10-29, landed GENOVLR (df9618d95)

Exact generic beats a concrete candidate reached by widening (C++/C# non-template tiebreak on the deduced parameter types,
free, method and implicit-this calls); `nullptr` to two pointer overloads is ambiguous; `void*` no longer picks an object
pointer overload. Literal expressions deduce their C type (`1.0+2.0` -> double, `true+false` -> int); a shift whose right
operand is wider keeps CFlat's own wider type (`1 << 2L` -> long, see below).

Pre-existing, kept (expression typing, not ranking): CFlat types `1 << 2L` as long (clang int); `1.5 + x` with int x is
float (clang double); char/u8/short variable arithmetic is not promoted to int; `'a' + 'b'` into a generic is refused.
Kept by main-session decision: `f(const int*)` + `f<T>(T*)` with an `int*` picks f(const int*) (clang: the generic).

## Leftovers (no ruling needed; clang accepts, CFlat refuses)

- `f(char*)` + `f<T>(T)` with a string literal: ranking ignores pointee const, so f(char*) wins and is then refused.
- Named arguments into a generic (`f<T>(T a, T* b)` called `f(b: q, a: l)`): inference is positional.
- Generic-only `T get<T>(T* p)` from a `const int*`, and `h<T>(T** p)` from a `const int**`.
- UFCS `p.f()` with a free generic `f<T>(T*)` reports "function 'f' is not known".
