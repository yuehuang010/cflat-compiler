# function<> C ABI leftovers after T77: narrow C callback returns at -O2, extern move-array count

Found by T77 review 2 (2026-10-09). Both are pre-existing: master 2c5fb5b3a and the T77 build fail the same way.

1. A CFlat callback returning `short`, passed to C and called there, returns the wrong value at -O2: 82 instead of 0. clang returns 0. The callback entry probably lacks the signext/zeroext RETURN attribute, so at -O2 the C caller reads the upper bits. Repro: scratch/repro_keep/t77/rev_t77_r2_narrowret.cb + rev_t77_r2_door.c.
2. An `extern` CFlat definition with a `move Elem*` parameter, bound to `function<int(Big, move Elem*)>` and called with a 3-element array, runs 1 destructor instead of 3 (exit 42). An extern definition has the C ABI, which has no hidden count slot, so the callee falls back to scalar cleanup. Repro: scratch/repro_keep/t77/rev_t77_r2_excount.cb.

Fix direction:
- Item 1: apply GetNarrowIntExtPolicy to the RETURN slot of reverse thunks and of the extern definitions themselves.
- Item 2 needs a decision: either refuse `move T*` params on `extern` (C ABI) definitions, or give extern definitions the counted entry as well. Since T77, every function<> C signature carries the count explicitly.
