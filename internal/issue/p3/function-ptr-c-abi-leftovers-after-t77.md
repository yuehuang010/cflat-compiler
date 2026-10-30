# function<> C ABI leftovers after T77: extern move-array count

Found by T77 review 2 (2026-10-09). Both are pre-existing: master 68bfae05f and the T77 build fail the same way.

1. FIXED by FPABI (70c49a598): narrow-return callbacks to C and C++ carry clang's return extension via the reverse ABI thunk.
2. An `extern` CFlat definition with a `move Elem*` parameter, bound to `function<int(Big, move Elem*)>` and called with a 3-element array, runs 1 destructor instead of 3 (exit 42). An extern definition has the C ABI, which has no hidden count slot, so the callee falls back to scalar cleanup. Repro: scratch/repro_keep/t77/rev_t77_r2_excount.cb.

Fix direction:
- Item 2 needs a decision: either refuse `move T*` params on `extern` (C ABI) definitions, or give extern definitions the counted entry as well. Since T77, every function<> C signature carries the count explicitly.
