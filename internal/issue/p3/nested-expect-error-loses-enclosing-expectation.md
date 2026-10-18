# A nested expect_error loses the enclosing expectation

Summary: an expect_error block nested inside another expect_error block: after the inner block catches its error, the
outer expectation is gone, so the outer's own error later fails the compile ("Undefined variable outerMissing") instead
of matching. Same on master and after const phase 4 (Sol c04 review 4, PRE-EXISTING).

Repro: scratch/repro_keep/const_c04/rev4/ee_nested_control.cb (immediate errors only); ee_nested.cb (deferred #8 variant).

Fix direction: expectation state is a single slot; make it a stack (arm pushes, scope end / catch pops) so the outer
expectation is restored when the inner one is consumed. The phase-4 expectation scope ids (ArmExpectScope) already give
each scope an identity to key the stack on.
