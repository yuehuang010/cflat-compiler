# Imported macro constant leftovers after T98

T98 (2026-10-10) made imported macro constants prvalues and folded enum/macro constants in brace
elements. Its reviews (Sol r1, r2) found these shapes failing on master too (pre-existing, not chased):

- A global variable or function whose name collides with an imported macro: identity resolution
  picks the wrong symbol (clang: the macro wins at the preprocessor, the global is unreachable by name).
- Enumerators of an anonymous enum, or of an enum inside an inline namespace, used as macro values
  or brace elements; enum-valued and bool-valued macros.
- A comma expression whose last operand is a macro constant (`(f(), M)`) is not a prvalue.
- `auto&& r = M;` syntax is not accepted.
- A macro expanding to a dereference (`#define RDEREF (*p)`) - clang: lvalue.
- C headers: errno-like macros and a transitively included INT_MAX fail to bind.

Repros and evidence: scratch/repro_keep/t98/ (reviews t98_review1.md, t98_review2.md list the probes).
Fix direction: each item is small; compare value AND chosen overload against clang++ -std=c++20.
