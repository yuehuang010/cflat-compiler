# A temporary closure arm in a ternary leaks its captured state

Found by T188 Opus r1 (2026-10-14); same on master. `m ? mk(5) : closureVar` (mk returns a capturing Lambda) leaks the temp's
captures at declaration, argument, return, call and assignment positions: 998-1000 leaks / 1000 iterations. Repros
scratch/repro_keep/t188_rv1/lk_*.cb (_b = baseline two closures, _f = with a function arm). Fix direction: the unselected/selected temp arm needs the
same drop the non-ternary temp closure gets.
