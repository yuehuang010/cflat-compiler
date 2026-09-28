# Debug: clang PushDeclContext assert when a late-parsed template body is materialized on demand

Found 2026-09-28 by the perf/prepass-fold Debug gate (pre-existing on master 78f08179, no source change).
Debug exe (assertions-enabled LLVM) on 23 of 116 `Test/errors/err_cpp_*.cb` fixtures, e.g.
`x64/Debug/cflat Test/errors/err_cpp_ref_qualifier_receiver.cb -i Test/library -o scratch/rq.out` ->
`Assertion failed: (DC->getLexicalParent() == CurContext && "The next DeclContext should be lexically
contained in the current one."), function PushDeclContext, file SemaDecl.cpp, line 1386`, exit 134,
AFTER every expect_error already printed PASS. Release runs the same path with the assert compiled out.
Crash-report stack (lldb's unwinder stops at the `.cold` frame, use the .ips):
`Sema::PushDeclContext <- Parser::ParseLateTemplatedFuncDef <- LazyBodies::ParseOne <-
LazyBodies::MaterializeReachable <- CxxIncrementalGroup::EmitDemandCompanion <- EmitCxxDemandCompanion
<- LLVMBackend::EmitCxxDemandCompanions <- LinkCxxCompanionModules <- Compile`.
Other asserting fixtures include err_cpp_alias_specialization, err_cpp_struct_tpl_arg_incomplete_list,
err_cpp_struct_lvalue_emplace, err_cpp_move_into_callee_use_after, err_cpp_struct_lvalue_sink.
Root cause direction: the demand companion re-enters `ParseLateTemplatedFuncDef` for a late-parsed
member template while `Sema::CurContext` is not the template's lexical parent (the Interpreter's
current chunk TU / a previous chunk's context), so the DeclContext push is out of order; in Release
the body is parsed under the wrong context (semantics unverified - could mis-resolve names).
Fix direction: in LazyBodies::ParseOne save/restore Sema::CurContext (Sema::ContextRAII to the
function's lexical DeclContext, or parse late bodies through `Sema::PerformPendingInstantiations`
style entry that sets up the context) before calling ParseLateTemplatedFuncDef; then re-run the Debug
sweep of Test/errors/err_cpp_*.cb (main-session baseline set: 23 fixtures exit 134) to zero.
Per CLAUDE.md an LLVM assert reached from user input also needs a proper LogError once root-caused.
