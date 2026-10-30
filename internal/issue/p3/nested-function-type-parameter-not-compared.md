# Function pointer ABI-class pointee crossings remain open

**NEEDS RULING (box 5, 2026-10-27, FNABI r1 parked):** an exact-pointee rule (clang: `u32*` vs `int*`, LP64 `long*` vs
`i64*`, `char*` vs `u8*` are distinct) breaks the pinned leg Test/test_function_ptr.cb ~3629
`function<int(char*)> gcp = onCharPtrSigned; spellCallU8Ptr(gcp)` ("Signedness-only crossings - the isolated axis",
together with by-value u32/int and u64/i64 legs). doc/LANGUAGE.md says function-type identity is exact, as in C.
Question: (a) pointees exact (clang/C), that leg becomes an err_ leg, by-value signedness crossings stay accepted; or
(b) pointee signedness crossings stay accepted (char*/u8*, u32*/int*), only width/type-identity crossings
(LP64 long*/i64*) refuse; or (c) close as working-as-intended. Branch fix/fnabi (worktree cflat-fix-fnabi) holds the
(a) implementation, uncommitted; report scratch/briefs/fnabi_report.md there.

Summary: Nested function-type parameter and return components are fixed by T500. The separate ABI-class crossings below remain open.

Also open (outside the T427 ruling): u32*/int* and LP64 long*/i64* pointees still cross under the ABI-class rule; clang refuses.

Repro shape: bind `function<void(u32*)>` to a `void(int*)` function, and on LP64 bind `function<void(long*)>` to a `void(i64*)` function.

Fix direction: determine whether these pointer-pointee ABI classes require an additional exact comparison, preserving T427's signedness and target-width rulings.
