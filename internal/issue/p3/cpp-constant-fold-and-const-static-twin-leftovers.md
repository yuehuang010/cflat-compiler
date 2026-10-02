# Leftovers after ST4 (C++ entity lookup): constexpr-call constant initializers, const receiver + static twin

Found by the ST4 re-review (2026-10-01). Neither is a regression (master refuses both).
Repros + oracles: scratch/repro_keep/st4_followups/.

1. `constexpr int answer() { return 7; } template<class T> inline const int value = answer();`
   reading `value<int>` is refused ("not initialized by a constant expression"); clang reads 7 and
   static_assert/constinit accept it. The fold guard (CClangExtract.cpp ~3203, used ~3223/~3800) uses
   HasSideEffects, which conservatively flags the constexpr call. Use the constant-evaluation result
   (EvaluateAsInitializer / checkForConstantInitialization) to decide; keep `(++counter, 27)` refused.
   Related: a plain `inline const int plain = answer();` is now read live instead of folded (correct
   value, different binding).
2. M has `static f(int)`, `f(double)`, `f(double) const`; `const M m{}; m.f(2)` must pick the static
   f(int) (clang 102); refused, candidate list shows only f(M*,double). Const-twin recursion in
   LLVMBackend_Overloads.cpp ~3236/~3404 runs before the mixed static/instance ranking.
Also seen, pre-existing: constexpr reference variables refused; const instance-twin calls fail.
