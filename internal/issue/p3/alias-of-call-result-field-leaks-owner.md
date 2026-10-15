# Owning call-result temp behind a raw pointer local leaks (T205 pointer path)

Summary: T256 (2026-10-15) fixed `alias` declarations: `alias int x = mk()->v;`, `alias int x = *mk()->heap;`,
`alias int x = idA(mk()->v);`, `alias int* p = mk()->heap;` and ternary arms now extend the owning receiver temp
to the alias's scope end (one destructor per iteration); a by-value argument (`alias int x = byval(mk()->v);`)
dies at statement end. Still open: a non-alias raw pointer local keeps the pre-existing no-free path (T205).

Repro: `for (...) { int* p = mk()->heap; s += *p; }` with a destructor counter -> 0 destructors on master and
after T256 (probe matrix: scratch/t256_matrix.md cells 13, 21). Also `int* q = c ? mk()->heap : nullptr;`.

Fix direction: needs a ruling first. A raw pointer's lifetime is not tracked (memory-safety ruling: borrowed
pointer lifetimes untracked), so extending the owner would need proof that the pointer's target lies inside
the temp. Leak, not use-after-free, until then.
