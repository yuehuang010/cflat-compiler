# Alias return of a by-value param: leftovers after B21 and V2

Summary: B21 (2026-09-29) fixed `U f(U w) { return w; }` (an inferred 'alias' return of a by-value param) handing back the callee's own dead copy: such params are passed as a pointer to a caller-owned copy slot, a temporary argument transfers to the result, named arguments stay non-owning aliases. W12 (2026-09-30) fixed the mixed named+temp argument case (the temp is destroyed exactly once per call via a run-time drop flag, the declared local stays an alias borrow, nested calls keep the inner temp alive, alias-return-with-temp results into an owning param / list.add are refused) and the function-value refusal location/wording. Probes: scratch/repro_keep/b21/ and scratch/repro_keep/w12/ (rev11 = hdr.cb + run_r3.sh; p/ + h/ = review probes with a corrected counting header - the rev11 hdr.cb counters miscount moved-from shells, use h/hdr.cb).

RULING 2026-09-30 (maintainer): no type-level ABI marker and no address-taken fallback; alias-return functions stay refused as function values (message/location fixed in W12).

V2 (2026-10-01) fixed items 1-5 of the previous list: named alias into an owning sink / list.add,
alias stored into an owning array element and a chained declaration then consumed are refused with the
alias diagnostic; `extern` (C linkage) definitions keep the C ABI with no alias inference (ruling
2026-09-30); a prototype followed by an inferred-alias definition links.

T212 (2026-10-14) fixed the forwarded frame-local alias gap below and V2 item 3. Alias-return checks now follow alias arguments through forwarding calls; owning stores also check alias results through phi/select values, dereferences, and container indexers. The requested safe-forwarding and legal-owning-store acceptance legs remain green.

Open:

1. (P3) Named alias keeps a stale shallow copy: `T x = f(a); a.u = new int(9);` then x reads freed memory (master too). A storage-rebinding attempt in V2 broke the mixed alias-return legs and was removed.
2. (P3, Windows) test.bat has no `// cflat-twin-args:` equivalent, so test_operators.cb runs only at -O0 there.
3. (P3, pre-existing) a NAMED alias local stored through a raw `*p` (`Box y = pick(a,b); *p = y;`) is
   still accepted: core hpc/btree.cb lookup's `*out = found` relies on that (out is an alias by contract),
   so T212 refuses only a direct alias-call result there. Needs a ruling on the btree contract first.
4. (P3) The '.copy()' steer in the alias diagnostic does not compile for a type with a unique field (BaT).
5. (Needs ratification) V2 made an extern (C linkage) callee own its by-value owning params (C ABI, ruling
   2026-09-30), so the call site moves the argument: `ef(a)` then reading `a` is refused "use of moved
   variable 'a'" (same rule as a declared `move` param). Alternative would be a caller-side copy.
6. (P3, pre-existing on master, leaks not crashes; probes scratch/repro_keep/v2/rv2/) a conditional rebind
   of an alias local leaks the new value (r2.cb, r3b.cb, r5.cb - deliberate leak-over-double-free trade);
   a non-extern function value called with a temp argument leaks it (e3o.cb).
8. (P3, pre-existing on master, rc 134; T216 review) a by-value string param rebound only through an
   `alias` local still frees the caller's buffer: `int f(string s) { alias string a = s; a = "zzzz"; return
   s.length(); }` (scratch/repro_keep/t216_aliasloc_repro.cb). T216's entry copy keys on whole-name stores;
   CollectAddressAliasWholeNameStores tracks `&param` aliases but not `alias T a = param`.

RULED 2026-10-01 (maintainer): item 5 RATIFIED - an extern "C" callee owns its by-value owning params; `ef(a)` moves `a` and a later read is "use of moved variable". No caller-side copy.
