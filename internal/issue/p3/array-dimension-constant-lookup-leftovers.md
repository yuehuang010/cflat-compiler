# Local const does not make an array dimension a constant (ruling needed)

Found by T206 (2026-10-14) from the T193 review. Items 1 and 3 of that review are closed: a function name
or other non-integer dimension is refused, and a non-const namespace int sizes a local array from its own
value (guard leg `t206_ns_nonconst_vla_not_file_const`).

What remains:
- A local `const int K = 5;` used as `int[K] a = default;` is a run-time-sized array (VLA). The alloca count
  is a load of the innermost K (5), so the size is right and the lookup order is right, with or without a
  file-scope `const int K = 1` or a namespace. But CFlat's `sizeof` on a VLA is the element size (4, the
  convention pinned by the T114 and T118 legs in Test/test_basic.cb), so `sizeof(a) / sizeof(int)` reads 1.
  clang++ -std=c++20 reports 5 (sizeof 20): a const local with a constant initializer is a constant expression.
- Repro: `int main() { const int K = 5; int[K] a = default; return (int)(sizeof(a) / sizeof(int)); }` - CFlat 1, clang 5.

Ruling needed: does a local `const` integer count as a constant expression for an array dimension? CFlat
drops `const` everywhere (const-unenforced ruling), so today no local is constant. If the answer is yes:
fold a const local with a constant initializer in FoldCompileTimeIdentifier (cflat/MainListener.h, where a
scoped local returns nullopt) and emit a fixed array; add a leg next to t206_ns_nonconst_vla_not_file_const.
If no: close this item and keep the VLA convention.

## Ruling 2026-10-15 (item 2): deferred to const-for-variables
Keep the VLA convention for now (`const` is still unenforced, so `K` may be reassigned and folding would be unsound).
The maintainer's next language feature is enforced `const` for variables; once it lands, a const local with a constant
initializer folds to a fixed array dimension (clang parity, `sizeof` = 5). Do not fold before that feature lands.
