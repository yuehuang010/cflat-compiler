# nullptr into a C++ `const std::nullptr_t&` param passes a null reference

Summary: `nref(nullptr)` with `inline int nref(const std::nullptr_t& p)` lowers to
`call @...nrefERKDn(ptr null)` - the REFERENCE itself is null. clang materializes a nullptr_t
temporary and passes its address. Values look right today only because comparing a nullptr_t does
not load it. Same for named `nref(p: nullptr)`. Pre-existing on master; found by T52 round 5
(2026-10-06). T52 already fixed `nref(0)` / `nref(ZERO)` (they use a pointer-sized temp).

Repro: scratch/repro_keep/t52_rev/rev4_t52_p/; `--symbol-dump-ir function:main`.

Fix direction: route a `nullptr` argument at a `const std::nullptr_t&` / `std::nullptr_t&&` param
through the same pointer-sized null temporary T52 uses for integer null constants.
