# Function pointer variable cannot bind to C++ bool parameter

Summary: A CFlat function pointer variable passed to a C++ `bool` parameter is rejected.
Repro: `function<int(int)>` variable passed to `cpp ob(bool)`.
Master vs expected: master reports no matching overload; clang accepts and returns 3. Data pointers and plain function names are accepted.
Found: T452 r1 opus review, 2026-10-24 box, PRE-EXISTING
