# C flexible / zero-length array member: sizeof/alignof/move leftovers after A6

Summary: these are the P3 leftovers from the A6 round-3 review (2026-09-28). A6 decays `p->data` to `T*` in every value context. Probes are in scratch/repro_keep/a6r3/, copied from the review worktree.
- `sizeof(z->data)` on a zero-length `int data[0]` is refused, but clang C11 accepts it and gives 0. Master gave 4.
- `sizeof(t.data)` and `sizeof((*b).data)` through a `.` receiver are not refused and give 8, where clang errors. Master also gives 8.
- `alignof(a->data)` gives 8, but clang's `_Alignof(int[])` gives 4.
- `int* p = move a->data;` traps at scope exit (rc 133), because `p` adopts an interior pointer. The same happens with any raw local moved into a pointer. Explicit move of a raw pointer should probably not make it owning; check the explicit-move ruling.
