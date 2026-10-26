# Named brace initialization: non-throwing Counted leak (T470 leftover)

Named brace initialization `{ c = move cc, t = s }` into a struct with C++ class fields leaks one `Counted`
on the NON-throwing path. Pre-existing on master; not on the unwind path, so T508 did not touch it.

Repro: `scratch/t508/item3.cb`. clang++ -std=c++20 oracle: `scratch/t508/item3_clang.cpp`
(live=0, cl=0, tl=0, dtors=4). CFlat master and T508 post-fix: `live=1 cl=1 tl=0 dtors=5` (one Counted never
destroyed). The struct is `Rx { t470.Counted c; t470.TC t; int z; Rx() { z = 3; } }` with the header
`scratch/t508/cpp_t470_braceunwind.hpp`.

Found: T470 r1 opus review, PRE-EXISTING. Fix direction not investigated; check the move-into-named-field
path (`EmitOneFieldInit` drop-old + `EmitForeignCxxValueIntoSlot`) before editing.

Resolved by T508 (removed from this file): replacement-unwind double destroy for nested `Outer.u.t` with a
throwing copy (fixed in `MainListener_Aggregates.cpp` second loop, shared field GEP), and heap
`RF* v = new RF { t = s }` with a throwing copy (fixed in `MainListener_Expressions.cpp` brace-init branch:
heap block plus completed-object unwind).
