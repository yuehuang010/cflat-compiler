# Alias return of a by-value param: leftovers after B21

Summary: B21 (2026-09-29) fixed `U f(U w) { return w; }` (an inferred 'alias' return of a by-value param) handing back the callee's own dead copy: such params are passed as a pointer to a caller-owned copy slot, a temporary argument transfers to the result, named arguments stay non-owning aliases. W12 (2026-09-30) fixed the mixed named+temp argument case (the temp is destroyed exactly once per call via a run-time drop flag, the declared local stays an alias borrow, nested calls keep the inner temp alive, alias-return-with-temp results into an owning param / list.add are refused) and the function-value refusal location/wording. Probes: scratch/repro_keep/b21/ and scratch/repro_keep/w12/ (rev11 = hdr.cb + run_r3.sh; p/ + h/ = review probes with a corrected counting header - the rev11 hdr.cb counters miscount moved-from shells, use h/hdr.cb).

RULING 2026-09-30 (maintainer): no type-level ABI marker and no address-taken fallback; alias-return functions stay refused as function values (message/location fixed in W12).

Open, each measured on the W12 commit and master:

1. (P2) Named alias passed straight to a sink: `baSink(baTPick(a, b, true))` with both arguments named compiles and aborts (rc 134) at -O0 and -O2 (master too; p/fpNamedSink.cb). The owning-param refusal only fires when an argument is a temp.
2. (P2) Alias value stored into an array element: `BaT[2] arr; arr[0] = baTPick(a, baTMk(3), c);` is not refused and aborts for temp and named forms (master too; p/arrS.cb, p/arrN.cb). Fields and owning locals are refused.
3. (P2) Chained declaration then consume: `BaT x = baTPick(...); BaT y = x; baSink(y);` (or `l.add(y)`) aborts for temp and named forms (master too; p/declChainSink.cb, p/declChainList.cb). The chained declaration is accepted on purpose (err_move expects the error on the store, not the declaration), but `y` is then consumed as if it owned the value.
4. (P2) CFlat function defined `extern` (C linkage): `extern U ef(U w) { return w; }` called with `mk(5)` prints 77 instead of 5 at -O0 and -O2 - C linkage keeps the old by-value ABI, so the dead-frame return remains.
5. (P3) A prototype followed by an inferred-alias definition gives an undefined symbol at link time (master too).
6. (P3) Named alias keeps a stale shallow copy: `T x = f(a); a.u = new int(9);` then x reads freed memory (master too).
7. (P3, Windows) test.bat has no `// cflat-twin-args:` equivalent, so test_operators.cb runs only at -O0 there.
