# A function-type parameter that is itself a function type is not compared

Summary: `function<int(function<int(void*)>)> h = takesCharCb;` where takesCharCb takes `function<int(char*)>` compiles
(also with char* vs int*); clang++ -std=c++20 refuses. A component that is a function type is never marked known, so
ComponentsProvablyDiffer (cflat/LLVMBackend_Lookup.cpp) cannot prove the mismatch.
Master vs expected: master and T427 accept; expected the function-pointer signature mismatch refusal.
Also open (outside the T427 ruling): u32*/int* and LP64 long*/i64* pointees still cross under the ABI-class rule; clang refuses.
Fix direction: recurse into function-type components with the same exact rule T427 applies to pointees.
Found: T427 r1 review (2026-10-24), PRE-EXISTING.
