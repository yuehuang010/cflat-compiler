# Remaining statement-temp join cases (p1)

T241/T242/T245 closed the direct, join-arm, later-defined void / non-void and C++ declared callee shapes.
T251 fixed the two field-pointer use-after-free cases: unwind cleanup now carries the deferred release
gate, and the pre-call walk follows a field GEP through `??` / `?:` arms.

Remaining from T242 review 1 (repros in scratch/repro_keep/t242_rev/):
- (leak) `f(vn ?? new E(5))` into an earlier-defined read-only CFlat `f(void*)` leaks the C++ class temp.
- (accepts-invalid) a `void*` join `vn ?? new E` passes to an `E*` param and `c ? new E : F*` to a `void*`
  param (CFlat and C++ callees); clang refuses all four.

From T251 review 1 (repros scratch/repro_keep/t251_rev/cases/):
- (trade-off) a callee that stores into the caller's out-param and then throws now keeps (leaks) the temp
  on unwind (d.cb o1.1); master freed it.
- (compile, master too) `&(c ? (e ?? new E(5)) : e)->v` and `&(c ? x : new E(6))->v` fail with
  "Undefined variable v."

From T260 review 1 (pre-existing on master; regenerate with scratch/repro_keep/t260_rev/rev_t260_opus_probe.sh):
- (compile) also `&(c ? new E(1) : d ? new E(2) : new E(3))->v` and `&(c ? (e ?? new E(5)) : (f ?? new E(6)))->v`
  fail "Undefined variable v.".
- (use-after-free) a C callee from an imported `.c` file that keeps the pointer: the temp is freed after the
  call (single-`new` controls too) - the C-callee policy treats it as read-only.
- (leak) an indirect `function<>` callee that only reads a two-`new` join leaks on both paths (conservative
  unknown-callee policy, same as the single-`new` control).
