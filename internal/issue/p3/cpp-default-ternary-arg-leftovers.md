# C++ member operator with a same-type cast of `default` as the right operand leaks

Left after T453 (2026-10-24). `ad + (cpptw.Twin)(default)` where `cppt453.Adder` has a by-value member
`operator+(cpptw::Twin)` (fixture Test/library/cpp_interop_twin.h, namespace cppt453). Counters encoded as
value*1e7 + ctor/copy/move/copy-assign/move-assign/dtor digits + live delta (bridge_t299_pack_result):

- clang++ -std=c++20: 1000010 (one ctor, no copy/move, one dtor, nothing live).
- master before T453: 1110021 (extra copy and move, one allocation leaked).
- after T453: 1010011 (copy gone; the default slot is still moved into the by-value parameter and one
  allocation leaks).

Repro: scratch/repro_keep/box1022/t453/a2.cb (compile with -i Test/library). The nested form
`cpptw.take(cpptw.Twin(ad + (cpptw.Twin)(default)))` matches clang (bridge leg "nested member operator default").

Root cause (T453 r2): the cast now returns the default slot as an rvalue (ParseCastExpressionImpl,
MainListener_Expressions.cpp ~20663), but the direct member-operand path classifies the stored rvalue as an
xvalue and marks it explicit-move for the by-value operator argument (MainListener_Expressions.cpp ~18188 and
~18938), so the parameter is move-constructed from the slot and the slot itself is never destroyed.

Fix direction: treat a default temp reaching a by-value C++ operator parameter as a prvalue - construct the
parameter in place (or hand the slot over as the parameter) instead of moving from it.
