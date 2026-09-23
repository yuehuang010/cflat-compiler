Bucket: p2 (C++ interop; left by fix/cpp-struct-lvalue-sink-refusal, 2026-09-23, macOS arm64 Release)

# Container sink gaps: emplace with a [cpp] struct lvalue stores a wrong element; set/map of a deleted-copy key crash the compiler

The lvalue-sink refusal (push_back / push_front / assign / resize with an lvalue whose copy
constructor is deleted) does not reach these shapes, measured on the pre-fix and fixed binaries alike:

1. `std.vector<T>.emplace_back(local)` and `emplace(pos, local)` with T a generated `[cpp] struct`:
   compiles, and the stored element is the base/default state (`v.front().forward(1)` reads 1, k = 0).
   Silent wrong value. With a plain move-only C++ class (cppas.MoveOnly) clang refuses it, as expected.
2. `std.set<T>.insert(local)`, `find` / `count` / `erase(local)` and `std.map<K,V>.insert(pair-local)`
   with a deleted-copy key: the compiler dies with SIGSEGV (both binaries).
3. `std.vector<T>(n, local)` count-plus-value constructor: SIGSEGV for the `[cpp] struct` element,
   "ambiguous constructors" then SIGSEGV for cppas.MoveOnly.

Repro corpus: scratch/lp2_*.cb of that worktree (compile with `-i Test`), matrix scratch/lp_matrix2.md.

## Fix direction

1: route the emplace forwarding pack through the same deleted-copy check (an lvalue bound to a
forwarding reference is a copy-construct of T from T&). 2/3: root-cause the crash first (lldb on the
compiler); a deleted-copy key most likely reaches a member-instantiation path that assumes a copyable
value type. Add LogError diagnostics where clang's own instantiation would refuse.
