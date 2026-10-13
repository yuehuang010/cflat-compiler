# C++ constructor with a `T*&` parameter binds a temporary, not the caller's pointer slot

Found by T47 (2026-10-09, all four Sol reviews). Pre-existing on master 608183756: rg6 exits 92 or 252 or 76 (garbage) instead of 6.

Repro: scratch/repro_keep/t47/rg6.cb + rg.hpp (`struct PRef { int*& q; explicit PRef(int*& x) : q(x) {} };`). The oracle is scratch/repro_keep/t47/rev_t47_ctor_oracle.cpp.
    int y = 6; int* yp = &y;
    auto pr = rg.PRef(yp);    // C++ binds q to yp itself
    return *pr.q;             // clang 6; CFlat reads a dead temporary

Root cause: unverified. The constructor-wrapper argument path probably materializes the pointer into a wrapper-frame temporary for the `T*&` parameter, so the stored reference dangles. T70 made `T*&` / `int*&&` call arguments bind the source slot for calls, so check that the ctor wrapper path (RequestCxxVariadicConstructor / ConstructThroughCxxCtorWrapper) uses the same binding.

Fix direction: bind the caller's lvalue slot at `T*&` ctor parameters, the same as at function calls. Since T47, reference members read as their referent, so a value leg in Test/test_cpp_interop.cb next to legs 1870-1896 can check `*pr.q == 6` and write-through.
