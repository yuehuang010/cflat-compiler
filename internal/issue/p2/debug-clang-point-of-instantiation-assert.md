# Debug cflat asserts in clang Sema compiling test_cpp_interop_bridge

PRE-EXISTING (found 2026-10-13 perf box, master e20aee5a3 Debug, assertions-on LLVM 23.1.0).
`x64/Debug/cflat -B Test/test_cpp_interop_bridge.cb -i Test/library -o x.bin` aborts:
`Loc.isValid() && "point of instantiation must be valid!"` (DeclTemplate.h:1968,
setPointOfInstantiation). Release silently continues with an invalid location.
Stack: ForwardRefScanner::ResolveForwardTypeArg -> TryRequestCxxType -> RunCxxTypeRequests ->
CxxIncrementalGroup::ParseRequest -> ExtractCxxIncremental -> cflat_cinterop (5 frames) ->
Sema::AddMethodCandidate -> TryCopyInitialization -> IsUserDefinedConversion ->
RequireCompleteTypeImpl -> InstantiateClassTemplateSpecialization -> InstantiateClassImpl.
Root cause direction: cflat_cinterop calls AddMethodCandidate with an invalid SourceLocation; pass a
valid location (the request's synthetic input location) to overload-candidate building.
