# Mixed-access overload set uses the most permissive access

Summary: Access is stored per function name at a permissive level, so a protected overload does not block selection.
Repro: `M { int operator-(double); protected: int operator-(int); } m - 2` and named `m.g(2)`.
Master vs expected: master accepts; clang rejects access to the selected protected overload.
Root cause hint: LLVMBackend_CInterop.cpp:25324,25615.
Found: T456 r1 opus review, 2026-10-24 box, PRE-EXISTING
