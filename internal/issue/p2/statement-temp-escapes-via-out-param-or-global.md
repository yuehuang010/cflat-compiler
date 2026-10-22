# Remaining statement-temp join cases (p1)

T241/T242/T245 closed the direct, join-arm, later-defined void / non-void and C++ declared callee shapes.
T251 fixed the two field-pointer use-after-free cases: unwind cleanup now carries the deferred release
gate, and the pre-call walk follows a field GEP through `??` / `?:` arms.

Remaining from T242 review 1 (repros in scratch/repro_keep/t242_rev/), re-measured by T366 (2026-10-20):
- (NEEDS RULING) `void*` joins: `vn ?? new E(5)` into an `E*` param and `c ? new E() : fp` (unrelated `F*`) into a `void*` param are
  accepted for CFlat and C++ callees; clang refuses all four. CFlat accepts the SAME joins in declarations, assignments and returns, so
  a call-only refusal would be a new rule. Ruling needed: keep these join conversions everywhere, or refuse them in every context
  (pointer-not-number ruling family). Matrix: scratch/repro_keep/box1021/t366/t366_matrix.md.
- (closed, not a bug) the earlier-defined read-only `f(void*)` leak did not reproduce: the repro sampled and reset the counters
  inside `show` while the statement temp was still live; measured after the full expression it frees once.

From T251 review 1 (repros scratch/repro_keep/t251_rev/cases/):
- (trade-off) a callee that stores into the caller's out-param and then throws now keeps (leaks) the temp
  on unwind (d.cb o1.1); master freed it.

From T260 review 1 (pre-existing on master; regenerate with scratch/repro_keep/t260_rev/rev_t260_opus_probe.sh):
- (use-after-free) a C callee from an imported `.c` file that keeps the pointer: the temp is freed after the
  call (single-`new` controls too) - the C-callee policy treats it as read-only.
- (leak) an indirect `function<>` callee that only reads a two-`new` join leaks on both paths (conservative
  unknown-callee policy, same as the single-`new` control).
