# Function-pointer call with brace argument crashes

Summary: A function-pointer call that passes a brace argument crashes the compiler.
Repro: `(*fp)({ z = 1 }, ...)` through `function<int(Arg,int)>*`; with named `Arg` local the program exits 138.
Master vs expected: master compiler exits 139 for the brace form and runtime exits 138 for named argument form; expected compile and run safely.
Found: T470 r1 opus review, 2026-10-24 box, PRE-EXISTING
