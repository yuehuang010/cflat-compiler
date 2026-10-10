# C++ class with a constructor template: unnamed-enum overload pick and trivially-copyable copy-init

Pre-existing on master (branch identical); found by the T54 review (2026-10-06). Each vs clang++ -std=c++20:
- A constant of an UNNAMED enum passed to a class with both an `int` ctor and an enum ctor template picks
  the int ctor; clang picks the template.
- Copy-init from a NON-const variable of a trivially copyable class with `template<class U> T(U&&)`
  copies the bytes (`T b = a`, `T b = k ? a : c`, `T b = arr[1]`); clang selects the forwarding template
  (better match than the const copy ctor for a non-const lvalue) and runs it.
Repros: scratch/repro_keep/t54_rev/.

Fix direction: overload ranking must consider the ctor template for an unnamed enum source and for a
non-const same-class lvalue (clang's [over.match.best]); keep trivial copies when the template is not
viable or not better.

Also refused on master (clang accepts; T54 review 3, 2026-10-06; repros scratch/repro_keep/t54_rev/rev3_t54_y/):
`r3.Bs d = dv1;` and `k ? dv1 : dv2` (derived local into a base with converting ctors), `r3.T1 d = {9};`,
`k ? pu : nullptr` into a class with a pointer ctor.

Also on master (T54 reviews 4-5, 2026-10-06; repros scratch/repro_keep/t54_rev/rev4/, rev5/):
- a converting ctor whose trailing default is not a literal (`int w = next()`, `int w = g`, `Tr t = Tr(7)`) is refused
  with "cannot initialize C++ class" (clang evaluates the default per construction);
- `D5(E, int = 0)` next to `D5(E)`: clang reports ambiguity, CFlat silently picks `D5(E)`;
- `q.T1 d = move ia[nx()];` refused ("needs a plain variable as its source"); clang constructs T1(int) from std::move.
