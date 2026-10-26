# `u.get()` on CFlat's own unique<S> calls S::get when S has get()

**RULED 2026-10-21:** `.` on unique<T> resolves unique's own members first, then forwards to T. See fix-issue-lessons.md digest.

Found by the T46 Opus review (2026-10-05), on master too: `scratch/rev_t46_cu.cb` in worktree cflat-fix-t46-smartdot.
For C++ smart pointers T46 makes `.` name the receiver's own member first. Needs a ruling: should `.` on CFlat
`unique<T>` (library struct per the unique ruling) also prefer unique's own members over forwarding to T?

## PARKED 2026-10-23 (T438): two follow-up questions before landing

Branch fix/t438-uniqdot (046b75cab, review CLEAN, patch kept in the worktree) implements own-member-first for `.` on every
CFlat struct with operator->. Opus review found two choices the ruling does not settle:
1. Name hiding vs shape fallback. The fix forwards to T when unique's own same-named member does not fit the arguments
   (`S::get(int)`: `u.get(2)` -> S::get, 42). C++ and T46 (std.unique_ptr) hide on the NAME: `p.get(2)` is "no overload".
   The fallback keeps Test/test_generics.cb:3093 `ll.get(0).get(0)` (unique<list<int>>) compiling; strict hiding would make
   it `ll->get(0)`. Also `u.reset(9)` with `S::reset(int)` ran on master (59) and is now refused (unique::reset(S*)).
2. `->` on unique now also picks unique's own member: `u->valid()` with `S::valid()` returns 1 (was 3); `u->get()` is
   refused (was 7). This matches CFlat's documented Box-> rule (MainListener_PostfixExpression.cpp:1917, test_operators
   "Box-> own method") but not C++ (`std.unique_ptr p->get()` reaches the pointee) or the operator-dot-forward reading
   `u->x` == `(u.operator->())->x`.
Options: (a) land as is (shape fallback, Box-> rule for unique too); (b) strict name hiding for `.` (edit the 3 test_generics
legs to `->`) and keep `->` forwarding for unique; (c) other.
