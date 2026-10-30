# function<> C ABI leftovers after T77: extern move-array count

Found by T77 review 2 (2026-10-09). Both are pre-existing: master 68bfae05f and the T77 build fail the same way.

1. FIXED by FPABI (1cc0ed5d2): narrow-return callbacks to C and C++ carry clang's return extension via the reverse ABI thunk.
2. An `extern` CFlat definition with a `move Elem*` parameter, bound to `function<int(Big, move Elem*)>` and called with a 3-element array, runs 1 destructor instead of 3 (exit 42). An extern definition has the C ABI, which has no hidden count slot, so the callee falls back to scalar cleanup. Repro: scratch/repro_keep/t77/rev_t77_r2_excount.cb.

Fix direction:
- Item 2 RULED 2026-10-29 (main session, from precedent; maintainer may veto): REFUSE, no counted entry (extern = C ABI, 2026-09-30; the count lives only in the CFlat `move T*`
  ABI, 2026-10-25). Binding an extern definition with a `move T*` parameter to a `function<>` type (count-carrying since T77)
  is a located type mismatch naming the extern; a direct call passing precise array provenance (`new T[n]`) into an extern
  `move T*` parameter is refused like R9 option 1; a scalar `new T` stays accepted. The repro becomes an err_ leg.
