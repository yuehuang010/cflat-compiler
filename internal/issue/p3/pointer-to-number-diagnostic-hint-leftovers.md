# Pointer -> number store refusal: diagnostic hint leftovers

Summary: P2I (2026-09-29) refuses every implicit pointer -> number store (declaration init, `=`, field store, designated / positional brace init, array element, compound assignment, ternary / `??` arms, pointer arithmetic results, string literals; bool exempt) with "a pointer is not a number; use an explicit cast '(T)p' ...". The refusals are right; some hints are not. Probes: scratch/repro_keep/p2i/ (main checkout).
- C++ reference member into a number: `int a = h.r;` (h.r is a C++ `int&` member) is refused (correct - master stored the address), but the hint suggests `(int)h.r`, which compiles and stores the address again. Suggest `*h.r` like the "cannot be reseated" diagnostic does.
- Floating destinations: the hint suggests `(double)p` / `(float)p`, and that cast fails module verification. A pointer has no floating conversion: say so instead of suggesting a cast (and make the explicit cast a clean error).
- The source type is spelled "pointer" for `p + 1`, `q ?? p`, `c ? p : nullptr` and `nullptr`; spell the real type ('long*', 'nullptr').
- A 2-D array row `int x = m[0];` says it converts 'int' to 'int'; it should name 'int[2]'.
