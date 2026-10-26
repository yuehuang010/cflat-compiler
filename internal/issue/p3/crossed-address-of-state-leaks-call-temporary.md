# Address-of temporaries leak through non-storing calls and two-owner pointer difference

Summary: owning call temporaries behind `&` are not destroyed at full-expression end in two shapes; master leaks identically.
Repro C2 (non-storing scalar callee): `int x = 1 + use(&mk()->v);` - d=0 after the statement, clang++ destroys once (d=1). No test leg (a leg would pin the leak).
Repro P (two owners, pointer difference): `int v = (mk()->heap + 2) - mk()->heap;` - d=0, clang++ destroys both (d=2). No test leg.
Repro S (storing callee, KEEP): `int x = 1 + store2(&mk()->v);` retains the owner on purpose (d=0, value 6). Never trade this leak for an early free.
Master vs expected: master matches the C2 and P results above; expected destruction at full-expression end for C2 and P.
Found: T485 r3 (2026-10-24), PRE-EXISTING for C2 and P.

Related (PRE-EXISTING, early free, worse than a leak): `long n = store3(mk()->heap) - q2;` where store3 saves its argument
frees the temp at statement end on master and the branch (d=1, the saved pointer reads garbage). Master ends temps passed
directly as pointer call arguments at statement end regardless of the callee storing them. Found: T485 r2 review.
Note: the T485 single-owner pointer-difference path (IsPointerDifferenceOperand) changed no probed outcome besides keeping
`alias int* r = mk()->heap - sp->n` retained; trim it when C2/P are fixed.
