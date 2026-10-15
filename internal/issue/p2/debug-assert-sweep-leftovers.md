# Debug cflat asserts left after T192/T194 (Debug sweep of Test/test_*.cb)

Found by the T194 Debug sweep (2026-10-14; Debug exe with assertions-on LLVM, `x64/Debug/cflat -B Test/<f>.cb -i Test/library -o x`).
Release silently continues past each. Re-sweep on current master first (T192 fixed the clang point-of-instantiation assert that
three cpp interop tests hit in that sweep, which branched before T192 landed).
- Test/test_collection_leaks.cb: SIGTRAP in CopyWrittenCopyableParams (CFlat-side assert or __builtin_trap - get the stack).
- Test/test_core.cb: LLVM assertion in a constant-expression cast (invalid cast opcode/operand in a ConstantExpr build).
Fix direction: backtrace each (lldb --batch -o run -o bt), root-cause; a user-reachable bad input gets a LogError, a compiler
bug gets fixed. Guard: the Debug compile of each file.
