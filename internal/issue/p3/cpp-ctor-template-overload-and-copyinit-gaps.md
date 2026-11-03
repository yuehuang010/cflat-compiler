# C++ class converting-constructor gaps (T54 reviews)

Pre-existing on master (branch identical); found by the T54 review (2026-10-06). Repros: scratch/repro_keep/t54_rev/.

Also refused on master (clang accepts; T54 review 3, 2026-10-06; repros scratch/repro_keep/t54_rev/rev3_t54_y/):
`r3.Bs d = dv1;` and `k ? dv1 : dv2` (derived local into a base with converting ctors), `r3.T1 d = {9};`,
`k ? pu : nullptr` into a class with a pointer ctor.

Also on master (T54 reviews 4-5, 2026-10-06; repros scratch/repro_keep/t54_rev/rev4/, rev5/):
- a converting ctor whose trailing default is not a literal (`int w = next()`, `int w = g`, `Tr t = Tr(7)`) is refused
  with "cannot initialize C++ class" (clang evaluates the default per construction);
- `D5(E, int = 0)` next to `D5(E)`: clang reports ambiguity, CFlat silently picks `D5(E)`;
- `q.T1 d = move ia[nx()];` refused ("needs a plain variable as its source"); clang constructs T1(int) from std::move.

CTORTPL (2026-10-30 box 7) fixed the unnamed-enum pick (all ctor paths) and non-const same-class copy-init via the forwarding template.
Left (Opus review 2, repros scratch/repro_keep/ctortpl/ + rv3.hpp):
- unnamed enum + literal `nullptr`, `rq.PF(rq.EA, nullptr)`, still picks the int ctor (clang 101 2, cflat 1 1); an `int*` variable works.
  The nullptr-literal candidate path runs before / skips the unnamed-enum clang-resolution return (p5.cb).
- a ctor template deleted for enums (`template<class U> PI(U) = delete`) next to an int ctor: clang errors, cflat picks the int ctor (p4.cb; master same).
