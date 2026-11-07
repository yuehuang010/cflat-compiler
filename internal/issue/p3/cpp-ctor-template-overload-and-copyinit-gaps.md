# C++ class converting-constructor gaps (T54 reviews)

Pre-existing on master (branch identical); found by the T54 review (2026-10-06). Repros: scratch/repro_keep/t54_rev/.

Also refused on master (clang accepts; T54 review 3, 2026-10-06; repros scratch/repro_keep/t54_rev/rev3_t54_y/):
`r3.Bs d = dv1;` and `k ? dv1 : dv2` (derived local into a base with converting ctors), `r3.T1 d = {9};`,
`k ? pu : nullptr` into a class with a pointer ctor.

Also on master (T54 reviews 4-5, 2026-10-06; repros scratch/repro_keep/t54_rev/rev4/, rev5/):
- a converting ctor whose trailing default is not a literal (`int w = next()`, `int w = g`, `Tr t = Tr(7)`) is refused
  with "cannot initialize C++ class" (clang evaluates the default per construction);
- `q.T1 d = move ia[nx()];` refused ("needs a plain variable as its source"); clang constructs T1(int) from std::move.

The unnamed-enum/nullptr selection, deleted enum constructor-template selection, and defaulted-parameter constructor ambiguity residue from CTORTPL/T54 are fixed in the CTORTPL2 worktree. The non-literal trailing-default, move-source, and derived-to-base copy-init items above remain active.
