# Array dimension from a C header #define crashes; negative global dimension has no diagnostic

PRE-EXISTING (T114 review 1, master and fix).
1. `#define DIMQ 3` from an imported C header used as a global, field or local array dimension crashes
   the compiler (exit 139). Repro: scratch/repro_keep/t114/hcase.cb (+ hdr.h).
2. A negative or overflowing GLOBAL dimension gets no diagnostic on master; object emit fails with
   "invalid number of bytes" (T114 round 2 is adding a LogError for <= 0 / over-limit dims).
