# Debug --check intermittently asserts M->isMaterialized() in FlushPendingFunctionDeclarations

PRE-EXISTING (found 2026-10-13 perf box, master e20aee5a3 Debug; master and the perf branch fail the
identical 532 tests). `x64/Debug/cflat Test/errors/err_alias_return.cb -i Test/library --check`
aborts on some runs, not others: `M->isMaterialized()` (Value.cpp:479). 463 of the 532 Debug
test.sh failures are this assert, and the test.sh discovery pass (many files in one --check) hits
it within 4 files, so `test.sh Debug` cannot run on macOS. Release passes silently.
Stack (main thread): Value::users <- LLVMBackend::FlushPendingFunctionDeclarations <-
CreateStructType <- MainListener::ParseClassDefinition <- ProcessPendingInstantiations <-
ParseFunctionDefinition. Intermittent + garbage parent module = an llvm::Value held across its
erasure (likely a parked PendingFunctionDeclaration or a RepairOpaqueReturn* site list keeping a
Function*/call that trial codegen erased). Fix direction: hold those as llvm::WeakVH / AssertingVH
and skip nulls; build Debug, loop the repro until it fires under lldb to confirm which list.
