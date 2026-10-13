# C++ constructor narrowing check guesses template viability from spellings

Found in T78 reviews 2-5 (pre-existing on master, not regressed). Repros: scratch/repro_keep/t78/.

## Summary
`CxxCtorSetRequiresNarrowing` (cflat/LLVMBackend_Overloads.cpp) decides whether a constructor
call narrows by scanning candidates itself. A constructor template counts as viable when arity
fits and a '*' parameter meets a pointer or null-constant argument. It cannot see:
- template constraints (`sizeof(T)==1`, `enable_if`, concepts): `Constraint(short)` + a template
  constrained away accepts int 257 (rev_t78_r2_constraint.cb); clang picks `short`, so CFlat
  should refuse the narrowing.
- deduction failure: `NullDeduction(short,int)` + `template<class T> NullDeduction(T*,double)`
  with `(0, 1.5)` - clang cannot deduce T from 0 and narrows 1.5 into the non-template; CFlat
  accepts (rev_t78_r4_null_deduction.cb).
A spelling-level "dependent" guard was tried in T78 round 5 and backed out: it refused a viable
non-deduced `typename Identity<T>::type*` template (false refusal is worse than false accept).

## Fix direction
Validate the constructor clang actually selects (deduced specialization signature) with
ClassifyScalarArgConversion, instead of pre-judging candidates by spelling.
