# Tuple passed to a scalar parameter through a function value fails module verification

Summary: `function<int(int)> p = f; (int,int) t = (3,4); p(t);` emits no CFlat error; the compile stops with "Module verification
failed". The direct call `f(t)` is refused with "no overload". Found: tuplevar Opus review 1, 2026-11-01, PRE-EXISTING (master same).
Repro: scratch/repro_keep/tuplevar/rev_tuplevar_preexisting_tuple_to_int.cb.
Fix direction: the function-value argument loop (MainListener_PostfixExpression.cpp, near the tuple-literal / tuple-value checks)
must refuse a tuple argument at a non-tuple parameter (and a scalar at a tuple parameter) with a located LogError, like the overload path.
