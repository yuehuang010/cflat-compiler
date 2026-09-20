# C++ variadic forwarding pack `A&&...` copies lvalue arguments

## Summary
`template<class... A> int vsum(A&&... a)` called with lvalues does not forward them by reference: the callee mutates
copies. A single `U&&` forwarding parameter binds an lvalue by reference, so the two are inconsistent. Pre-existing.

## Repro
    // header: template<class... A> int vbump(A&&... a) { ((++a), ...); return 0; }
    int a = 1; int b = 2; ns.vbump(a, b);   // a and b stay 1 and 2; C++ gives 2 and 3

## Root cause
cflat/CClangExtract.cpp (~1042) tests `getAs<RValueReferenceType>` on a `PackExpansionType`, which fails, so a pack
records forwardingReferenceParameters = 0 and takes the by-value path in LLVMBackend_CInterop.cpp (~6459).

## Fix direction
Peel PackExpansionType before the rvalue-reference check; spell each pack element `T &` or `T &&` by the argument's
value category in the wrapper generator. Also: an explicit `f<int>(a)` should report the CFlat rvalue-reference
diagnostic instead of clang's no-match. Add legs to Test/test_cpp_interop_template.cb.
