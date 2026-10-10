# C++ operator `move` operand: leftovers after D8

Summary: these are the leftovers from the D8 report (2026-09-28). D8 binds `move d` straight to a `T&&` or by-value operator operand, the same as `f(move d)`. Probes are in scratch/repro_keep/d8/.
- A template `operator&=(U&&)` is still refused ("no overload for compound assignment"); clang accepts it.
- A ctor-call prvalue into a BY-VALUE operator param: clang elides it (mv0 dt2); cflat gives mv1 dt3.
- `P1 p2 = p & P1(4)` / `p & mkP1(4)`: the class result takes one extra move (mv1, dt+1 vs clang). Master is identical.
- `c &= move a[1]` marks nothing moved, while `f(move a[1])` marks the whole array moved. Values match clang.
- RULED 2026-10-09 (maintainer): BLOCK - a `const T&` parameter is a borrow, so `move d` into an operator that only takes `const T&` is refused exactly like `f(move d)` into a `const T&` param ("transfers nothing"). Was: `move d` into an operator that only takes `const T&` is accepted (clang-equal, and master's behaviour), but `f(move d)` into a `const T&` param is refused ("transfers nothing"). Calls and operators disagree.
From the D8 review (2026-09-28); probes are in scratch/repro_keep/rv8/:
- An `&`-qualified member operator refuses even a plain lvalue `c &= d`, on master too. With `move d` the message now says "rvalue receiver", which is misleading; clang gives c=401, d=-7.
- `c &= (cond ? move a : move b)` picks the `const T&` overload and steals nothing. Clang gives c=201, a=-7; cflat gives c=3, a=2, the same as master.
- A `const T&&` or non-stealing `T&&` operator after `move d`: reading `d` afterwards now errors "use of moved variable", the same as `f(move d)`. Master picked the wrong overload here. This is RULING-adjacent: consistent with calls.
