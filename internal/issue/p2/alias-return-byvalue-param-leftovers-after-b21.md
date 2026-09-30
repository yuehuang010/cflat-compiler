# Alias return of a by-value param: leftovers after B21

Summary: B21 (2026-09-29) fixed `U f(U w) { return w; }` (an inferred 'alias' return of a by-value param) handing back the callee's own dead copy: such params are now passed as a pointer to a caller-owned copy slot, a temporary argument transfers to the result (runtime slot compare with several sources), named arguments stay non-owning aliases, and -O0 / -O2 agree (test_operators.cb twin-runs at -O2 via `// cflat-twin-args:` in test.sh). Using such a function as a function value (function<>, function pointer, C thunk, interface vtable slot) is refused. Probes: scratch/repro_keep/b21/ (main checkout).

Open, each measured on the B21 branch and master:
1. (P2) Named and temporary argument mixed: `U x = f2(a, mk(3), false)` returns the temp but x reads freed memory (78 at -O0 and -O2; master 78 at -O0, crash at -O2). The temp's slot must live until the consumer takes it (or the enclosing scope ends).
2. (P2) CFlat function defined `extern` (C linkage): `extern U ef(U w) { return w; }` called with `mk(5)` prints 77 instead of 5 at -O0 and -O2 - C linkage keeps the old by-value ABI, so the dead-frame return remains.
3. (P3) The function-value refusal is reported after the whole walk, so its location is the last statement parsed, not the use; for an interface conversion (`IP ip = s;`) the text "as a function value" does not describe the spelling. Record the use location where the function address is taken (see firstCallLocation_ for the poisoned-call pattern).
4. (P3) A prototype followed by an inferred-alias definition gives an undefined symbol at link time (master too).
5. (P3) Named alias keeps a stale shallow copy: `T x = f(a); a.u = new int(9);` then x reads freed memory (master too).
6. (P3) Supporting such functions as function values needs the ABI marker on function<> / fn-pointer types (ruling: is that worth a type-level marker, or should inference refuse to make a function alias when its address is taken).
7. (P3, Windows) test.bat has no `// cflat-twin-args:` equivalent, so test_operators.cb runs only at -O0 there.
