# `(std.errc)22` as the first mention of an imported C++ enum type: "cannot find the type"

`auto e = (std.errc)22;` with no earlier `std.errc.x` use fails "cannot find the type 'std.errc'"; after any
`std.errc.x` use it works. Same on master. Found by T49 (2026-10-05); probes in worktree cflat-fix-batch-t49
scratch/t49/ until removed.

## Fix direction

A C-style or functional cast's type operand naming an imported C++ enum should request/publish the enum type
lazily, the same way an enumerator access does.
