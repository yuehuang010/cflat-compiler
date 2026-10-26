# function<> value with an unrelated pointee param binds to a different function type

**NEEDS RULING (T427 parked 2026-10-23):** exact pointee matching conflicts with a deliberate existing leg, Test/test_function_ptr.cb:3166 `function<void(int*)> gp = takeVoidPtr;` ("Any two pointer types are one class under opaque pointers" - int*/void* crossing accepted), and Test/errors/err_overloaded_fn_name_no_match.cb:16 (expects `ambiguous overload ... void(void*)` between move int* / move double* overloads; exact matching reports no match instead). Question: (a) follow clang - function types match exactly, void* is not a wildcard, rewrite both legs; or (b) keep the opaque-pointer crossing and refuse only unrelated non-void pointees (char* vs int*, A* vs B*), or (c) leave as is. Round-1 patch (option a): scratch/repro_keep/box1022/t427/. Worktree cflat-fix-t427-fnpointee kept.

Filed 2026-10-19 from the c14 round-2 Sol review. PRE-EXISTING on master.

`pick(function<int(char*)> f)` called with a `function<int(void*)>` value (or `function<int((const void)*)>`) is
accepted; clang++ -std=c++20 refuses (function types must match exactly; `int(*)(void*)` does not convert to
`int(*)(char*)`). Repro shape: scratch/rev_c14_sol_r2 probes in the c14 worktree (`unrelated_base`, gitignored).

Fix direction: function-type parameter compatibility must compare each component's pointee TYPE exactly, like the
c14 exact-match rule for pointee const (LLVMBackend_Overloads.cpp function-type viability, FuncPtrPointeeConstDiffers
neighbour). Acceptance: the repro refused with the existing argument-mismatch text; same-type binds unchanged.
