# Alias return of a by-value param: leftovers after B21 and V2

Summary: B21 (2026-09-29) fixed `U f(U w) { return w; }` (an inferred 'alias' return of a by-value param) handing back the callee's own dead copy: such params are passed as a pointer to a caller-owned copy slot, a temporary argument transfers to the result, named arguments stay non-owning aliases. W12 (2026-09-30) fixed the mixed named+temp argument case (the temp is destroyed exactly once per call via a run-time drop flag, the declared local stays an alias borrow, nested calls keep the inner temp alive, alias-return-with-temp results into an owning param / list.add are refused) and the function-value refusal location/wording. Probes: scratch/repro_keep/b21/ and scratch/repro_keep/w12/ (rev11 = hdr.cb + run_r3.sh; p/ + h/ = review probes with a corrected counting header - the rev11 hdr.cb counters miscount moved-from shells, use h/hdr.cb).

RULING 2026-09-30 (maintainer): no type-level ABI marker and no address-taken fallback; alias-return functions stay refused as function values (message/location fixed in W12).

V2 (2026-10-01) fixed items 1-5 of the previous list: named alias into an owning sink / list.add,
alias stored into an owning array element and a chained declaration then consumed are refused with the
alias diagnostic; `extern` (C linkage) definitions keep the C ABI with no alias inference (ruling
2026-09-30); a prototype followed by an inferred-alias definition links.

Open:

1. (P3) Named alias keeps a stale shallow copy: `T x = f(a); a.u = new int(9);` then x reads freed memory (master too). A storage-rebinding attempt in V2 broke the mixed alias-return legs and was removed.
2. (P3, Windows) test.bat has no `// cflat-twin-args:` equivalent, so test_operators.cb runs only at -O0 there.
3. (P3, pre-existing on master, rc 133 aborts; probes scratch/repro_keep/v2/rev/) alias-return result
   stored where the alias check does not look: `baSink(k ? pick(a,b) : pick(c,d))` (both-alias-arm phi not
   matched by IsAliasReturnResult, u_tern2.cb); `*p = pick(a,b)` (u_ptrarr.cb, u_ptrplain.cb);
   `l[0] = pick(...)` and `l[0] = y` (list index store, u_listset.cb, u_listsetY.cb).
4. (P3) The '.copy()' steer in the alias diagnostic does not compile for a type with a unique field (BaT).
5. (Needs ratification) V2 made an extern (C linkage) callee own its by-value owning params (C ABI, ruling
   2026-09-30), so the call site moves the argument: `ef(a)` then reading `a` is refused "use of moved
   variable 'a'" (same rule as a declared `move` param). Alternative would be a caller-side copy.
6. (P3, pre-existing on master, leaks not crashes; probes scratch/repro_keep/v2/rv2/) a conditional rebind
   of an alias local leaks the new value (r2.cb, r3b.cb, r5.cb - deliberate leak-over-double-free trade);
   a non-extern function value called with a temp argument leaks it (e3o.cb).
