# Generic overload vs concrete overloads: needs-ruling picks and leftovers after GENOVL

GENOVL (1bb1f5139) lets a generic candidate join a native overload set with concrete overloads when no concrete candidate
resolves the call on its own, so every call master REFUSED and clang accepts now picks as clang (pointer, value, pair,
namespace, both declaration orders). Matrix and probes: scratch/repro_keep/genovl/ (genovl_report.md, genovl_review1.md,
review1/cases/).

## NEEDS RULING: master compiles with a different pick than clang (kept unchanged)

`f(double*)=2; f(float*)=3; f<T>(T*)=9` and `f(int)=1; f(long)=2; f<T>(T)=9` sets, both declaration orders:
- `void*` argument: CFlat 3, clang 9. `nullptr`: CFlat 2, clang ambiguous.
- `short` / `char` value: CFlat 1 (widening to f(int)), clang 9 (exact generic).
- two-parameter `short,char` and `int,long`: CFlat 2, clang 9.
- int -> bool, a long/double tie, variadic: CFlat 2, clang 9.
Question: should an exact generic beat a concrete candidate reached by a widening conversion (C++ rule), changing these
compiling programs?

## Leftovers (no ruling needed; clang accepts, CFlat refuses)

- `f(char*)` + `f<T>(T)` with a string literal: ranking ignores pointee const, so f(char*) wins and is then refused.
- Named arguments into a generic (`f<T>(T a, T* b)` called `f(b: q, a: l)`): inference is positional.
- Generic-only `T get<T>(T* p)` from a `const int*`, and `h<T>(T** p)` from a `const int**`.
- UFCS `p.f()` with a free generic `f<T>(T*)` reports "function 'f' is not known".
