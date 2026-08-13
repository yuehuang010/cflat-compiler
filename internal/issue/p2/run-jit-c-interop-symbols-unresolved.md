# `--run` cannot materialize imported C source symbols

## What

`--run` is intended to support a source importing a real C file, but currently fails to materialize
the imported C function. The AOT path compiles and links the source, but the ORC JIT only sees the
generated LLVM module and symbols exported by the current process. Prebuilt C libraries are a
separate, intentionally unsupported mode and are not covered by this issue.

## Repro

```text
x64/Release/cflat Test/test_c_interop.cb -i Test/library --run --nologo
```

Observed on macOS arm64 Release:

```text
JIT session error: Symbols not found: [ _c_add, _c_square, _ml_add, ... ]
--run: could not find 'main': Failed to materialize symbols
```

The same fixture passes through the ordinary AOT suite.

An isolated probe with only `import "import_c_probe.c"` also failed with unresolved
`_imported_add`, so this is not attributable only to the prebuilt library portion of
`test_c_interop`.

## Triage

Recommended first implementation target in q19, after the `--run` native-input contract is
settled. This is the shared object/library loading problem with the broadest interop coverage;
imported program adapters should reuse its supported-or-rejected symbol policy.

## Root cause

`LLVMBackend::JitRun` adds the generated IR module and a current-process symbol resolver, but it
does not add the object files produced for imported `.c` sources or the symbols from prebuilt
libraries. Those inputs are currently owned by the AOT link path.

## Fix direction

Add the object produced for each imported C source to the ORC JIT, including lifetime and
platform-specific symbol lookup rules. Do not broaden this issue to prebuilt `--c-lib` inputs;
those should be rejected clearly under `--run` as unsupported by design. Do not leave the current
unresolved-symbol dump as the user-facing behavior for supported source-C imports.

## Related

- `q19-run-jit-interop-and-diagnostics`
- `Test/test_c_interop.cb`
- `cflat/LLVMBackend_CInterop.cpp`
- `cflat/LLVMBackend_EmitAndLink.cpp` (`JitRun`)
