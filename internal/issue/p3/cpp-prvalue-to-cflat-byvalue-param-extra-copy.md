# A C++ class prvalue passed to a CFlat by-value parameter runs an extra copy constructor

`g(f())` where `f()` returns a C++ class with a user copy ctor and `g` is a CFlat function taking it
by value runs the copy ctor once; C++17 guarantees elision (0 copies). Spelled init `T x = f();`
and return through a CFlat function already make 0 copies. Pre-existing on master (the member
path behaves the same). Found 2026-09-27 reviewing fix/udc-return (scratch probes in
../cflat-fix-udc-return/scratch/rev_udc/ while that worktree exists).

## Fix direction

When the argument is a prvalue C++ record of the parameter's exact type, pass the temporary itself
as the parameter storage (move ownership of the temp into the callee's param slot) instead of
copy-constructing a second object. Assert copy counts via a static counter in a fixture class.

- (O2X review 2, 2026-09-29) The generated `[cpp] struct` override wrapper forwards a by-value non-trivial param to the CFlat body as an lvalue (MainListener_Aggregates.cpp ~1206), one extra copy: dtor count 3 vs clang 2, balanced with or without a throw. `std::move(p)` in the wrapper fixes it. Probe cflat-fix-o2x scratch/o2xrev2/.
