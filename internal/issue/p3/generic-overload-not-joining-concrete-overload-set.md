# A generic overload never joins a set that has concrete overloads

`f(double*)=2; f(float*)=3; f<T>(T*)=9; int* p; f(p)` - clang picks the exact generic (9); CFlat keeps a concrete
pick or refuses. 12 clang-valid calls (`A*`, `u8*` with a generic) are refused on master. Found by the T36 Sol
reviews (2026-10-05); cell set in worktree cflat-fix-t36-ptrarith scratch/rev_t36_152.log + review_boundary_*.cb.

## Fix direction

Native overload resolution should rank generic candidates (after deduction) together with concrete ones, C++
partial-ordering style: exact generic beats a conversion to a concrete candidate; a concrete exact match beats an
equally exact generic. Needs a matrix over both declaration orders before any change (ranking change = behaviour
change on currently-compiling programs - may need a ruling for cells where CFlat accepted a different pick).
