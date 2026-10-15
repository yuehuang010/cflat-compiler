# A ternary with a generic instantiation arm (wrap<int>) into a function slot miscompiles

Found by T188 Opus r1 (2026-10-14); same on master. `k ? t1 : wrap<int>` into function<int(int)> exits 133; `k ? cap : wrap<int>`
(cap a capturing Lambda) exits 138. Plain `Lambda g = wrap<int>;` and `function<int(int)> g = wrap<int>;` work, so the ternary
arm lowering of a generic instantiation name is wrong (likely not materialized as a function value in its arm block).
Repros: scratch/repro_keep/t188_rv1/gen_fn.cb, gen_k1.cb, gen_plain.cb, generic.cb. Related: p3/generic-function-name-as-pointer-argument.
