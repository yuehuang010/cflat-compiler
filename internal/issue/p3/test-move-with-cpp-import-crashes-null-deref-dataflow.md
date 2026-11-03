# Test/test_move.cb plus one C++ import crashes the compiler (SIGSEGV)

Found 2026-11-01 by movecopy r2 (Luna), PRE-EXISTING (master exe 02:33 crashes too, different frame).
Repro: prepend `import cpp "library/cpp_interop_freeoptpl.h";` to Test/test_move.cb, compile with `-i Test/library -o x.bin`
(copies: scratch/repro_keep/movecopy/test_move_import_master.cb, logs *_out.log / movecopy_r2_import_only.log). rc 139.
Fix tree: `movedf::ComputeRpoIndex` <- `nulldf::AnalyzeFunction` <- `RunNullDerefDataflow` <- `RunDeferredEndOfBodyChecks`
<- ParseFunctionDefinition. Master build: `llvm::Instruction::replaceSuccessorWith`. Wild addresses (0xb9, 0x6563727500000041 =
ASCII) point at a dangling BasicBlock / freed function body, likely a C++ demand / wrapper emission that erases or rewrites
blocks of a function the deferred end-of-body dataflow still holds. Bisect the test_move functions (keep the import) to a minimal
repro first; run the Debug (assertions) build for a precise assert.
