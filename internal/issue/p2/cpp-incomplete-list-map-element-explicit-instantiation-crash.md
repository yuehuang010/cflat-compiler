# std.list / std.map of a not-yet-defined [cpp] struct crashes the compiler (Release SIGSEGV)

## Summary

A [cpp] struct field of type `std.list<Leaf>` or `std.map<int, Leaf>`, declared before `Leaf`
is defined, crashes instead of reporting the "needs the complete definition of [cpp] struct"
diagnostic that the `std.vector` / `std.deque` spellings report
(`Test/errors/err_cpp_struct_tpl_arg_incomplete.cb`, `..._incomplete_deque.cb`).
Release: exit 139, no output. Debug (assertions-enabled LLVM 23.1):
`Assertion failed: (!D->isInvalidDecl() && "Cannot get layout of invalid decl!"),
function getASTRecordLayout, file RecordLayoutBuilder.cpp, line 3388`.

Distinct from the ItaniumMangle "unexpected statement kind" unreachable (fixed on
fix/cpp-debug-itanium-mangle): that one was cflat mangling a member signature; this one is
clang CodeGen emitting a deferred body.

## Repro

`Test/errors/err_cpp_struct_tpl_arg_incomplete_deque.cb` with `deque` -> `list` (import,
`std::list` in the expect text, `std.list<Leaf>` field); same with `std.map<int, Leaf>`.
Probe copies: scratch/dm_02_list_field.cb, scratch/dm_03_map_value_field.cb on that branch.

```
x64/Release/cflat --check -i Test/library scratch/dm_02_list_field.cb   # exit 139
x64/Debug/cflat   --check -i Test/library scratch/dm_02_list_field.cb   # assert above, exit 134
```

## Root cause (measured, lldb on Debug)

`MainListener::ParseDeclarationSpecifiers` -> `TryRequestCxxType("std.list")` ->
`RequestCxxForeignType(tentative=true, explicitInstantiation=true)` -> `RunCxxTypeRequests` ->
`CxxIncrementalGroup::ParseRequest` (source `typedef std::list<__cflat_user::Leaf> ...;
template class std::list<...>;`) -> `clang::Interpreter::Parse` -> `CodeGenModule::Release` ->
`EmitDeferred` -> `GenerateCode` of an instantiated member body -> `EmitCXXMemberCallExpr` ->
`CodeGenTBAA::getBaseTypeInfoHelper` -> `getASTRecordLayout` on an invalid record (the
incomplete element made a node/value record invalid). The request is tentative, but clang's
CodeGen consumer runs on the explicit instantiation before cflat can see the error.

## Fix direction

Before an explicit-instantiation request reaches CodeGen, detect that the instantiation (or a
record it needs) is invalid - e.g. parse the typedef first and check the specialization /
its members for isInvalidDecl, or suppress CodeGen for a TU that produced errors - and fall
into the existing "needs the complete definition" refusal. Keep the LLVM/Clang assertion on.
