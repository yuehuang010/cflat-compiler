# C++ interop: the move/ternary leftovers from the T339 identity-cast fix

Found by the T339 round 2 review (box 2026-10-17). Both were already present on master before T339 and are still open after it.
Repros: `scratch/repro_keep/box1017/t339_r2rev/` (`probe.hpp` plus `.cpp` clang twins). Counts are value/born/copies/moves/dead/chosen.

1. When a moved temporary is cast inside a call argument, the move construction is skipped:
   `rv.take((rv.Box)(move rv.make()));` gives 1/0/0/1 in cflat; clang++ -std=c++20 gives 2/0/1/2 (both pick `take(Box&&)`).
   T339 fixed the same shape in a declaration initializer (`Box a = (Box)(move make());`, leg 33907). The argument
   position still takes the early-return path (`move_temp_arg.cb`).
2. When a ternary selects its prvalue arm inside a cast initializer, cflat adds an extra copy:
   `Box a = (Box)(true ? make() : b);` gives 7/3/1/0/3 in cflat versus 7/2/0/0/2 in clang (`mixed_true.cb`).
   The prvalue arm is joined into a temporary and copied, where clang initializes `a` from that arm directly. The false-arm
   (lvalue) form matches clang.
