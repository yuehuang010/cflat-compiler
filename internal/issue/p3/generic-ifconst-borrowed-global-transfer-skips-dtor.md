# Generic if-const borrowed global transfer skips destructor

Summary: A borrowed-global transfer in a generic `if const` path fails to destroy the transferred resource.
Repro: `scratch/repro_keep/box1022/t443/generic_ifconst_resource.cb`.
Master vs expected: master dtor count is 0; clang `unique_ptr` oracle count is 1.
Found: T443 r4 sol review, 2026-10-24 box, PRE-EXISTING
