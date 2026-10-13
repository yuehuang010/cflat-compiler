# Fields and array elements holding C++ classes are destroyed in forward order

Summary: a CFlat struct with several C++ class fields, and an array of C++ class objects, destroy
their members first-to-last (`~1 ~0 ~3`) where C++ destroys in reverse construction order
(clang++ -std=c++20: `~3 ~0 ~1`). Observable for RAII types whose destructors depend on each other
(lock guards, scoped handles). Counts are balanced; only the order is wrong.

Found: T117 review 1 (2026-10-11), pre-existing on master. Probe header: T117 worktree
scratch/rev_t117_rv.h (event log D0/Cn/K/M/~n).

Fix direction: emit field destructors in reverse declaration order and array element destructors
from the last element down (struct dtor synthesis and array cleanup loops); check the partial-unwind
path too. Ruling reference: cflat-struct C++ field lowering rule A (lower like C++).
