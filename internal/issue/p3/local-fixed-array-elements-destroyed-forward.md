# A local fixed array destroys its elements in forward order; C++ destroys them in reverse

Summary: `It[3] arr;` at scope end runs element destructors 0,1,2; clang++ runs 2,1,0 ([class.dtor], array elements in
reverse). Struct members (T488) and fixed-array members inside a struct are already reversed.
Master vs expected: master and T488 forward; expected reverse index order.
Fix direction: reuse the T488 reversal helper at the local array cleanup walk (LLVMBackend_OwnershipTemps.cpp ~6718/6729).
Found: T488 batched review (2026-10-24), PRE-EXISTING.
