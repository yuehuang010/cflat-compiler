# A lambda literal bound to a Lambda<> slot is never signature-checked

Summary: `Lambda<int(int)> L = (int a, int b) => { return a + b; };` compiles; `Lambda<function<int(int)>()> L = () => { return add2; };`
(add2 takes two ints) runs garbage (exit 185 master, 177 after T186). The lambda's return type is inferred, so its `return` never
reaches CoerceToFuncPtrReturn's bind rule (T186), and the lambda-to-slot bind compares no signature.
Repro: scratch/repro_keep/t186_rv1/l_lambda_arity.cb, scratch/repro_keep/t186_rv1/r_lambda_ret_fn.cb (found by T186 Opus r1, 2026-10-14; same on master).
Fix direction: at the lambda-literal -> Lambda<>/function<> bind, compare the lambda's parameter list (and inferred return) with
the slot signature using the same bind rule as named functions (fewer-parameter accept stays ruled, LLVMBackend.h
DescribeFuncPtrBindMismatch); the returned named function inside then meets the declared return type.
