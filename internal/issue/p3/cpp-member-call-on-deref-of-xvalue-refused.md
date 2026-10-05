# `(*std.move(opt)).value()` refused: "cannot dereference a value without addressable storage"

clang accepts a member call on the dereference of an xvalue optional (`(*std.move(opt)).value()` - operator*
`&&` overload, then a member call on the resulting xvalue); CFlat refuses with "cannot dereference a value without
addressable storage". Same on master 33866560 and after T9 (which typed `T&&` class returns as xvalues of T).

Fix direction: give the `std.move(opt)` xvalue operand addressable storage (the source object's address) before
the unary `*` overload, as a named operand has.

Found by: T9 review, fix timebox 2026-10-02 (probe scratch/rev_t9/ in the T9 worktree).
