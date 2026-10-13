# Static locals of C++ class type: three leftovers after T93

Found by T93 review r1 (2026-10-10), all pre-existing on master. Probes: scratch/repro_keep/t93/
(rev_t93_r1.sh drives them; compare cflat vs clang).

1. Derived-to-base: `static B g = D(x);` and `static B g = c ? D(1) : D(2);` are refused with the
   unrelated "cannot cast an aggregate value - a fixed array decays..." message (rev_t93_qdplain.cb,
   rev_t93_qdtern.cb). clang slices via B's copy ctor. Either slice like clang or refuse with a
   slicing-specific message.
2. `static T g = c ? a : b;` with two lvalue arms calls the copy ctor 0 times; clang calls it once
   (rev_t93_lv.cb). The static aliases or bit-copies an arm.
3. An exception from a temporary's noexcept(false) destructor inside the static's initializer does
   not propagate: cflat prints `3 3 made=1`, clang `-9 4 made=2` (rev_t93_cleanup.cb).
