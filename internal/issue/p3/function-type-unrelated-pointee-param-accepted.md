# function<> value with an unrelated pointee param binds to a different function type

Filed 2026-10-19 from the c14 round-2 Sol review. PRE-EXISTING on master.

`pick(function<int(char*)> f)` called with a `function<int(void*)>` value (or `function<int((const void)*)>`) is
accepted; clang++ -std=c++20 refuses (function types must match exactly; `int(*)(void*)` does not convert to
`int(*)(char*)`). Repro shape: scratch/rev_c14_sol_r2 probes in the c14 worktree (`unrelated_base`, gitignored).

Fix direction: function-type parameter compatibility must compare each component's pointee TYPE exactly, like the
c14 exact-match rule for pointee const (LLVMBackend_Overloads.cpp function-type viability, FuncPtrPointeeConstDiffers
neighbour). Acceptance: the repro refused with the existing argument-mismatch text; same-type binds unchanged.
