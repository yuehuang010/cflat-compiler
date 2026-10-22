# Alias-return string of a temp argument: METHOD callee dangles, function<> lambda leaks

Severity p1 (use-after-free, item 1). Found by the T357 review (2026-10-20); pre-existing on master (T357 scopes to free functions:
`!candidate.IsMethod`). Cases: scratch/repro_keep/box1021/t357_rev/ (G_method.cb, H_lambda.cb).
1. `struct S { string f(string s) { return s; } }; S o; string r = o.f("x".copy());` -> heap-use-after-free (ASAN), master and T357.
2. `function<string(string)> g = (string s) => { return s; }; string r = g("x".copy());` leaks one allocation, master and T357.
Fix direction: extend T357's call-site adoption (LLVMBackend_Overloads.cpp, aliasStringTempMayOwn) to method candidates and indirect
calls through function<>/Lambda values (where the callee's return-sink param facts are known or conservatively assumed).
