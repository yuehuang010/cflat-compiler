# Residue: imported test_move reaches a later unique reset refusal

The self-return adoption refusal at `b = makeIdResource(41)->self()` is fixed: owning-temp
adoption now accepts `InvokeInst` through `CallBase`, and the focused regression in
`Test/test_cpp_interop.cb` verifies the result stays live through the statement and is destroyed
once at scope exit.

The element-borrow reassignment refusal at `elemBorrowReassignedBox()` is fixed. Invoke creation now
retargets straight-line ownership facts to its known normal continuation; no block-terminator walk
is used. `ResolvePointerElementTypeName` also accepts `CallBase`, preserving arm types when calls
become invokes. Interop regression legs cover element rebinding, delete after an intervening call,
and an owning `??` interface return, each checking its value and destructor count.

With `import cpp "library/cpp_interop_freeoptpl.h";` prepended and a fresh cache, the imported
`Test/test_move.cb` copy now progresses past those refusals. It next fails at source line 20714 in
the `T207Res` test with `cannot reset unique<T207Res> 'owner' from borrowed value 'p' because the
source still owns it`. This remaining imported-test divergence is deferred after the three-root
cause limit for this round.
