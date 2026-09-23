Bucket: p3 (diagnostic quality)

# Explicit `f<int>(a)` on a forwarding-reference template reports clang's no-match, not the CFlat rvalue-reference diagnostic

Split 2026-09-23 from the variadic forwarding pack fix (fix/cpp-variadic-forwarding). With
`template<class... A> int vbump(A&&... a)` in Test/library/cpp_interop_tpl.h (namespace vfprobe
in the probe corpus), the explicit-argument call `vbump<int>(a)` with an lvalue `a` says
"no instantiation of C++ function template 'vfprobe.vbump<int>' accepts these argument types
(int) (clang: no matching function for call to 'vbump')", identical before and after the pack
fix. Explicit template arguments skip the HasCxxForwardingReferenceTemplate shortcut
(cflat/MainListener_PostfixExpression.cpp ~6525) so the CFlat-side rvalue-reference diagnostic
never fires. C++ also rejects `vbump<int>(a)` (int&& cannot bind an lvalue) - the accept/reject
answer is right; only the message is. Fix direction: when explicit template arguments make a
parameter `T&&` with T non-reference and the argument is an lvalue, report the CFlat
rvalue-reference diagnostic (the one the implicit path uses) instead of relaying clang.
