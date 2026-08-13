# `--run` smoke-test report - 2026-08-13

## Scope

Ran an ephemeral positive-test sweep using the macOS arm64 Release compiler and `--run`.
No tracked test harness or compiler files were changed. Logs and result files are under the
gitignored `scratch/run-smoke/` directory.

The compiler was built with `./cmake_build.sh release`. The ordinary AOT suite was also run as a
baseline.

## Results

| Check | Result |
|---|---|
| Ordinary AOT suite, `bash test.sh Release` | 647 passed, 0 failed, 8 skipped |
| `--run` sweep: 34 fixtures with `int main()` or `int main(int,char**)` | 30 passed, 4 failed |
| Additional special-entry fixtures: `test_allocators`, `test_stream`, `test_threadpool` | 3 passed |
| `--run` argv probe: `hello_imported.cb -- alpha beta` | exit code 3, as expected |
| `--init-local` followed by the passing smoke fixtures | passed |

The four failures are not ordinary language/codegen failures:

- `test_c_interop` cannot resolve symbols from imported C sources; the fixture also exercises a
  prebuilt library, which is intentionally outside the `--run` contract.
- `test_collection_leaks`, `test_list_ownership`, and `test_reflect` cannot resolve the C-backed
  HeapAudit functions used by their self-auditing code.

`test_program` was probed separately because it has no ordinary top-level `main`; its normal
program/thread machinery is not the problem. It fails when imported program adapters are
materialized, with unresolved `___imported_main_Hello` and `___imported_main_HelloC` symbols.
The latter is a real C source import.

The explicit `--heap-audit` option already rejects `--run` with a clear diagnostic because it
requires a linked C diagnostic object. The smoke failures above are source-level imports of that
same C-backed diagnostic module, so they expose the same JIT object-linking gap.

The isolated source-C probe (`import "import_c_probe.c"`) independently failed with unresolved
`imported_add`, confirming that the source-C leg is a real `--run` gap rather than only a
prebuilt-library limitation.

Prebuilt C libraries are not a proposed fix target: they are unsupported by design and should get
a targeted diagnostic when used with `--run`.

## Conclusion

The in-process JIT is viable for the ordinary portable suite: 30/30 compatible fixtures passed,
including ownership, generics, SIMD/HPC, synchronization, process, and threadpool coverage. The
remaining work was grouped in q19: add or explicitly scope JIT support for imported native
symbols, imported program adapters, and C-backed diagnostic fixtures.

## Triage recommendation

1. Preserve imported C source as supported, but add a targeted pre-JIT rejection for prebuilt
   `--c-lib`/package-library inputs.
2. Implement imported C source object loading and symbol resolution (`test_c_interop` source leg).
3. Then fix imported program adapters (`test_program`), reusing the source-C path for its C leg.
4. Leave HeapAudit parity for last. The existing `--heap-audit` restriction makes AOT-only leak
   validation an acceptable near-term disposition.

This ordering was recorded in the q19 triage table and keeps the 30/30 portable smoke set
independent of optional native-diagnostic parity.

## Follow-up - 2026-08-13

Implemented the first two work items:

- Imported C source objects are now loaded into ORC/LLJIT for `--run`.
- `import program` works for both imported CFlat and real-C program adapters.
- Prebuilt C libraries are rejected before JIT materialization with a targeted diagnostic.
- `test.sh --run` is an opt-in smoke mode; it passed 35/35 selected checks.

The normal suite remains unchanged and passed 647/647 runnable checks. The only remaining q19
item is the deliberately lower-priority HeapAudit/AOT-only gap.
