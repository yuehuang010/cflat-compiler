# A binary operator on a mutable field of a const C++ object picks the const overload

Found by the T335 round-2 review (2026-10-17); the same on master and the T335 branch.
`struct Item { int pick(); int pick() const; int operator+(int) { return 31; } int operator+(int) const { return 42; } };`
`struct Holder { Item normal; mutable Item mut; const Item fixed; };` with a const `Holder h`.
- `h.mut + 1` gives CFlat 42 (const overload) and clang 31: a mutable member of a const object is non-const.
- `h.mut.pick()` already matches clang. The member-call receiver path honours `mutable`; the binary-operator receiver path does not.
Repro header: scratch/repro_keep/box1017/t335_r2rev/more.hpp (the `h` object is declared on its line 3).
Fix direction: the binary-operator receiver constness (CxxConstReceiverKind or the operator ranking receiver) should apply the same mutable-field
exemption as the member-call path. T335 round 2 tried a receiver handoff here and regressed `cfhLocal.pc->only()` (test_cpp_interop ~14313), so
start from the member-call path's predicate instead.
