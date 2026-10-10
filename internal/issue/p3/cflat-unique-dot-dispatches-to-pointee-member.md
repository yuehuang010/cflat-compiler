# `u.get()` on CFlat's own unique<S> calls S::get when S has get()

Found by the T46 Opus review (2026-10-05), on master too: `scratch/rev_t46_cu.cb` in worktree cflat-fix-t46-smartdot.
For C++ smart pointers T46 makes `.` name the receiver's own member first. Needs a ruling: should `.` on CFlat
`unique<T>` (library struct per the unique ruling) also prefer unique's own members over forwarding to T?
