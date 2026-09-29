Bucket: A (crash / assert). Found 2026-09-28 by run A1 (Debug sweep); cross-target only.

# Debug: setVisibility "local linkage requires default visibility" in EmitExecutableElf (-p win64)

x64/Debug aborts on `Test/errors/err_cpp_import_cross_target.cb` only with `-o ... -p win64`:
`GlobalValue::setVisibility <- EmitExecutableElf <- EmitExecutable <- Compile`. A global with local
linkage gets a non-default visibility. Cross-target is deferred (macos-native focus, see
cross-target-compile-gaps.md), so p3; fold into that work. Fix direction: skip setVisibility for
local-linkage globals (or set default) in EmitExecutableElf.
