Bucket: p3 (C++ interop operators; found by the D5 round-2 review, 2026-09-29)

# A C++ operator<< / operator&& that returns a reference is refused

D5 made reference-returning C++ operators fold in the additive, multiplicative and bitwise (`& | ^`)
chains. A C++ `T& operator<<(T&, const T&)` or `T& operator&&(...)` returning a reference is still
refused, even with plain variable operands (`T r = a << b;`) - same on master. Side effect of D5: on
master `T r = c * c << a` compiled to a WRONG value (1, clang 10); it is now refused instead.
Probes: scratch/repro_keep/d5/rv/ (r2* files, round-2 review).

Fix direction: route the shift and logical fold levels through the same reference-result carry D5
added (ParseCxxFoldOperand / the shared carry helper in MainListener_Expressions.cpp).
