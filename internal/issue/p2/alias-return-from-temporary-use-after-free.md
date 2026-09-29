# Alias-returning function called with a temporary: use-after-free

Summary: `U f(U w) { return w; }`, where U is a struct with a unique field, is an inferred 'alias' return, meaning the callee hands back the argument.
- Calling it with a temporary, `U x = f(mk(5)); U y = mk(77);`, destroys the argument temp right after the call, so x points at freed memory.
- The program prints x=77: y reused the freed slot.
- Pre-existing on master; confirmed by the B13 review, 2026-09-29.
- Repro: scratch/repro_keep/b13r1/e2_U_tmp_heapcheck.cb, plus e1 U_c1_tmp_d.

The other alias sites are already refused with "cannot store an 'alias' value": `a = f(a)` and `return f(a)`.

Fix direction: refuse a declaration initialised from an alias return whose aliased argument is a temporary, the same way. Or materialise the temporary into the declaration: the ownership moves because nobody else holds it. Check which one the inferred-alias ruling in fix-issue-lessons.md picks before coding.
