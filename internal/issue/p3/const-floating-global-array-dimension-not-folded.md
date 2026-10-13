# Const floating global in an array dimension is not folded

Summary: `const double D = 1.0; int[(D > 0.0) ? 2 : 1] a;` gives sizeof 4 (local VLA path) where clang gives 8; as an inner dimension it is refused ("array dimension must be a compile-time constant", clang 16). Literal twins fold.

Repro: scratch/repro_keep/t114/rev_t114_r3/const_fp.cb, inner_const_fp.cb (T114 review 3).

Root cause: constFoldableGlobals_ admission (MainListener_Declarations.cpp ~8618) requires a ConstantInt initializer, and the other admission sites go through TryGetConstGlobalInt, so a const double global never enters the allowlists TryFoldConstInt honors for float loads (T114 made float loads honor them to stop folding MUTABLE globals).

Fix direction: admit const-qualified floating globals under their LLVM global names; keep mutable globals out; extend Test/test_basic.cb array legs.
