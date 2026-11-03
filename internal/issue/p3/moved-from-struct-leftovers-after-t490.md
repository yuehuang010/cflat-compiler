# Moved-from struct leftovers after T490

T490 (2026-10-24) made a moved-from plain CFlat struct (user dtor, no owning fields) dead at scope exit. Still open:

- (T418, master too) A by-value C++ class parameter consumed with `_ = move t` runs its dtor 3 times for 2 objects
  (`cfm(rettw())` -> stats 10130, clang 10120). The T490 r1 attempt (skip the release for parameters) fixed the count but
  deferred the release to function exit (stats inside the callee 10001 vs master 10010), unlike `_ = move` on a C++ local;
  backed out. Fix: keep the immediate release and disarm the parameter's later cleanup.
  T499 (2026-10-25) tried "callee destroys the parameter when its body always consumes it, caller skips": correct for
  direct calls, but two rounds failed on indirect calls - a function value keeps the first target's contract
  (`fp = A; if (c) fp = B;` leaks, a typed function<int(Val)> parameter destroys twice: 10240 vs clang 10120), and lambdas /
  generic function values skip the caller destroy without a callee destroy (leak). Backed out; master behaviour kept.
  RULED 2026-10-29 (maintainer): CALLEE DESTROYS, ALWAYS - every CFlat-ABI (non-extern) function destroys its own by-value
  ctor/dtor-bearing parameters on every target (MSVC convention, uniform); `_ = move t` = immediate release + local drop flag;
  one contract for direct and indirect calls, function values, lambdas and generics. Extern definitions keep the platform ABI.
  Supersedes the T499 "callee destroys only when the body always consumes" attempt. Conditional consumes (`if`, early return, loop, recursive: 20030 vs clang 20020) need the
  same contract plus the callee's conditional drop flag. Repros: scratch/repro_keep/t499/ (main checkout).
- Fixed in MOVEDTOR2: the heap-field path (`take(move p->inn)`) and every other by-value move source (local, parameter,
  local / nested / generic field, array element, `return move x`) now zero-fill the source and run its destructor on the
  zero state, matching a clang twin with a zero-filling move constructor (220-cell matrix scratch/movedtor2_matrix.md in
  the cflat-fix-movedtor2 worktree). MVFIELD's local field drop flags are removed (they skipped a destructor clang runs).
- Fixed in MOVEDTOR2: the T490 dead-shell rule and its raw-pointer / view exclusion are removed - a moved-from plain struct
  of any field shape runs its destructor once on its zero state (`~Buf(){ free(data); }` frees null, never twice).
Repros: scratch/repro_keep/t490/ (main checkout).
Found: T490 r1 review (2026-10-24).

## Value-type destructors always run (Fixed in MOVEDTOR2)

The 2026-10-30 all-zero-skip question (branch fix/movedtor) is answered by the ruling below and implemented in MOVEDTOR2:
zero-fill + unconditional destructor, no all-zero skip, no drained flag, no `int alive = 1;` sentinels; guard.cb exits 10.

RULED 2026-10-31 (maintainer): VALUE-TYPE DESTRUCTORS ALWAYS RUN; the owning pointers are the tracking. A move of a by-value object
(`S y = move x`, `take(move p->r)`, elements) copies it bitwise to the destination and zero-fills the source; the source's destructor
runs at its normal time on that zero state and must tolerate it (delete / free / unique of null are no-ops), as a moved-from C++ object
is destroyed. The compiler may elide a destructor only when it proves the body does nothing. No all-zero skip test, no drained flag,
no layout change. Only an OWNING POINTER move (`move g` of a `Guard*` / `unique<T>`) nulls the slot and runs no destructor. Supersedes
the 2026-10-29 "destructor skipped on an all-zero object" line, the T490 line of 2026-10-24 "a moved-from plain struct is dead, no
second destructor" (and the 2026-10-29 scope line widening it): a moved-from Guard decrements `locked` twice like clang, a live
field-less Guard runs once (guard.cb exit 10). What changes: fix/movedtor (worktree cflat-fix-movedtor) is reworked to zero-fill +
unconditional destructor, dropping the all-zero skip and the `int alive = 1;` fixture sentinels; MVFIELD's local drop flags are no
longer needed for correctness (keep only as an elision); fixtures pinning one destructor run per moved value (T490 legs, MVFIELD legs,
the raw-pointer / view exclusion legs) move to clang's counts. T418 (callee destroys by-value params, 2026-10-29) is unaffected.
