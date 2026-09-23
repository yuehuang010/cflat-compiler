# C++ header harvest asks CodeGen to emit a trivial destructor (Debug assert)

## Summary

Importing `Test/library/cpp_interop_tpl.h` aborts a Debug build (assertions-on LLVM) inside the
header harvest, in both incremental and legacy (`CFLAT_CPP_INCREMENTAL=0`) mode:

```
Assertion failed: (!DD->isTrivial() || DD->hasAttr<DLLExportAttr>()) &&
"Should not emit dtor epilogue for non-exported trivial dtor!", CGClass.cpp line 2010
```

Release has the assert compiled out and passes, so whatever CodeGen emits for that destructor there
is unchecked. Every Debug run of a test that imports this header dies here, which also blocks using
Debug to diagnose anything later in such a compile.

## Repro

```
import cpp "cpp_interop_tpl.h";
extern int main() { return 0; }
```

`x64\Debug\cflat.exe --check -i Test\library repro.cb` -> exit 0xC0000005 after the assert.

## Stack (Debug)

`CompileCHeader` -> `ExtractCHeaderClang` -> `CxxIncrementalGroup::HarvestHeader` ->
`ExtractCxxIncremental` -> `HarvestTranslationUnit` -> `ComputeCxxAbi` -> `EmitCxxDefinitions` ->
`CodeGenModule::Release` -> `EmitDeferred` -> `codegenCXXStructor` -> `EmitDestructorBody` ->
`EnterDtorCleanups` (assert).

## Root cause (not yet confirmed)

A destructor that clang considers trivial reaches `EmitDestructorBody` through the deferred-decl
queue. Suspect the implicit-special-member definition pass before `EmitCxxDefinitions`
(`DefineImplicitDestructor` + `MarkFunctionReferenced` in CClangExtract.cpp) or a
`HandleTopLevelDecl` of a trivial destructor: CodeGen expects trivial destructors to be skipped,
never emitted. Which record in `cpp_interop_tpl.h` triggers it is not identified yet.

## Fix direction

Find the destructor (bisect `cpp_interop_tpl.h`), then skip trivial destructors wherever cflat
forces emission. Debug `test.bat` should then get past this header.
