# `default` inside a ternary arm or cast in a C++ by-value call argument leaks a copy

Found by the T302 Opus reviews (2026-10-16), pre-existing on master; T302 fixed only a `default` that IS the call argument
(bare or parenthesized). Twin counters from Test/library/cpp_interop_twin.h (ctor/copy/move/dtor/live), cpptw.take(Twin)
from Test/library/cpp_interop_lastuse.h. Probe: scratch/repro_keep/wt_t302/rev_t302_opus_probe2.sh.

| Argument | CFlat (master = fix) | clang++ -std=c++20 |
|---|---|---|
| `take((cpptw.Twin)(default))`, `take((cpptw.Twin)default)` | `1/1/0/0/0/1/1` (copy, live 1) | 1 ctor, 1 dtor, live 0 |
| `take(c ? default : move tw)`, `take(c ? default : (default))` | `1/1/0/0/0/1/1` | accepted, live 0 |
| `take(c ? default : rettw())`, `take(c ? default : cpptw.Twin(4))` | refused "the '?:' arms handle ownership differently" | accepted |
Also: interface call `iface.take(((default)))` gives one extra move vs clang (balanced), and C++ ctor by-value param
`Holder((default))` is refused "'default' needs a known target type" (clang accepts).

Fix direction: treat a prvalue C++ class produced by a cast of `default` or by a ternary of prvalue arms as an owned
call-argument prvalue (one slot, no copy), as T302 does for the direct case in GenerateDefaultValue
(cflat/MainListener_Declarations.cpp ~2730); unify the ternary ownership classification with clang (both prvalue arms ->
prvalue result).
