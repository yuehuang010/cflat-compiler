# Lowered struct (C++ class field) move-sink leftovers after T295

Found by the T295 Opus review (2026-10-16), both pre-existing on master. L = CFlat struct with a C++ class field
(lowered), P = plain CFlat struct with a dtor (the control). Probe script:
scratch/repro_keep/wt_t295/rev_t295_opus_probe.sh.

1. `cp(move v)` - an explicit `move v` into a COPY parameter: P is refused "... 'move v' transfers nothing"; L is accepted
   and stays readable after. T295 deliberately marks L moved only for a `move` parameter.
2. A by-value L parameter forwarded into a move sink, `int fwd(L b) { mv(b); return 0; }`: P gets the laundering error, L is
   accepted (master and fix). With a later `b.id` read the fix reports "use of moved variable 'b'", so only the read-free
   shape is silently accepted.

Fix direction: route L through the same checks as P at the explicit-move-into-copy-param and by-value-param-into-sink sites
(T295 marking is in cflat/LLVMBackend_WinRT.cpp ApplyMoveParamTransfer ~1771).

## NEEDS RULING (T303 attempt, 2026-10-16, parked)
Luna's fix (patch scratch/repro_keep/wt_t303/t303_r1_parked.patch, report + matrix beside it) made L match P for both
items, but item 1 contradicts landed tests: Test/test_cpp_interop.cb T130 (`T130Parameter(move source)`, ~16624) and
Test/test_cpp_interop_bridge.cb "BV holder explicit move C++" (~6011) require `move v` into a by-value L parameter to
COMPILE and run the C++ move constructor (move=1, copy=0) - the C++ by-value rule (cflat-struct C++ field lowering rule A).
Question: for L (lowered, C++ by-value semantics), is `move v` into a by-value param (a) a real move-construct (keep
today, drop item 1; item 2 then likely also follows C++: the by-value param is an owned copy) or (b) "transfers nothing"
like P (rewrite the two landed legs)? Recommend (a): lowering rule A says L behaves like C++, and the move saves a copy.
