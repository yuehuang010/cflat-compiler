# `--run` cannot materialize imported program adapters

## What

The `program` construct itself works under `--run` in the tested allocator, stream, and threadpool
fixtures. However, a fixture containing `import program` fails when its imported entry adapters
are materialized.

## Repro

```text
x64/Release/cflat Test/test_program.cb -i Test/library --run --nologo
```

Observed unresolved symbols include:

```text
___imported_main_Hello
___imported_main_HelloC
```

`Hello` is imported from `hello_imported.cb`; `HelloC` is imported from the real C source
`hello_c_program.c`. The ordinary AOT suite passes this fixture.

## Triage

Second in q19. Resolve the native-input contract and the shared C/object symbol path first. Then
make the `.cb` adapter JIT-visible, and route the real C adapter through the source-C import path
rather than creating a separate one-off path. Prebuilt C libraries remain unsupported.

## Root cause

`import program` emits adapter references whose definitions or native objects are made available
by the AOT linker. The JIT module has the references but no corresponding ORC-visible definitions
for these imported entry points.

## Fix direction

Make `.cb` imported-program adapters part of the JIT-visible module and decide separately whether
real C imported programs are supported by loading their object into ORC. If either form is outside
the `--run` contract, diagnose it before JIT materialization and document that `program` support is
limited to in-module definitions.

## Related

- `q19-run-jit-interop-and-diagnostics`
- `Test/test_program.cb`
- `Test/library/hello_imported.cb`
- `Test/library/hello_c_program.c`
