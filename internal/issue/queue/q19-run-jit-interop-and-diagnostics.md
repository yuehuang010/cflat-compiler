# q19: JIT execution - interop and diagnostics

3 items. The ordinary in-process `--run` path works across the portable suite, but ORC/LLJIT
currently materializes only the generated LLVM module and symbols already present in the host
process. It does not yet provide the intended imported-C-source and imported-program resolution.
Prebuilt C libraries are intentionally outside the `--run` contract and are not an issue here.

## Shared root cause

`LLVMBackend::JitRun` installs a current-process symbol generator and adds the generated IR
module. Imported `.c` files and program-adapter symbols are handled by the AOT emission/link path
instead of being added to the JIT. C-backed diagnostic modules therefore remain unresolved too.
Prebuilt C libraries are deliberately not supported by `--run` and should be rejected clearly if
they are passed.

## Members

- `p2/run-jit-c-interop-symbols-unresolved` - imported C source objects are not available to
  `--run`; the source-C leg of `test_c_interop` fails during JIT materialization. Prebuilt C
  libraries are intentionally out of scope.
- `p2/run-jit-import-program-symbols-unresolved` - `import program` adapters are emitted as
  external symbols that the AOT linker supplies, but `--run` cannot materialize them;
  `test_program` fails for both the CFlat and C imported programs.
- `p3/run-jit-heapaudit-symbols-unresolved` - source-level HeapAudit users fail under `--run`
  because `diagnostic/heap_audit.c` is not linked. Either provide a JIT-safe audit backend or
  make the smoke classification and documentation explicit.

## Triage and recommended fix order

| Order | Item | Priority | Recommendation | Dependency |
|---|---|---:|---|---|
| 0 | Contract and diagnostics | P2 prerequisite | Preserve source-C imports as supported intent; reject prebuilt `--c-lib`/package-library inputs under `--run` with a targeted message. | None |
| 1 | `run-jit-c-interop-symbols-unresolved` | P2 | Add imported C source objects to ORC/LLJIT and make their symbols visible to the JIT. | Order 0 |
| 2 | `run-jit-import-program-symbols-unresolved` | P2 | Make `.cb` imported-program adapters JIT-visible; route imported C programs through the order-1 source-C path. | Orders 0-1 |
| 3 | `run-jit-heapaudit-symbols-unresolved` | P3 | Do not block the main JIT effort on leak-oracle parity. Keep HeapAudit fixtures AOT-only unless a JIT-safe audit backend naturally follows from order 1. | Orders 0-1, optional |

### Triage ruling

The recommended near-term implementation is order 0 followed by order 1: source-C imports are
supported intent and need to work in-process, while prebuilt libraries should receive a clear
unsupported-mode diagnostic. Then implement order 2 on top of the source-C path. Do not start with
HeapAudit: the public `--heap-audit` option already declares that it cannot run in-process, so
HeapAudit parity is a lower-value extension rather than the first correctness fix.

### Acceptance sequence

1. Add a targeted pre-JIT rejection for prebuilt C libraries and preserve source-C imports as a
   supported mode.
2. Re-run the 30-fixture portable `--run` smoke set and the AOT suite.
3. Make an isolated imported-C-source probe and the source-C legs of `test_c_interop` pass under
   `--run` before touching `test_program`.
4. Make the `.cb` imported-program leg pass, then route the real C imported-program leg through
   the same source-C object path.
5. Only then evaluate a JIT HeapAudit implementation; otherwise record the three HeapAudit tests
   as deliberate AOT-only coverage.

## Fix direction

1. Define the supported `--run` interop contract. Imported C source objects are supported and
   should be added to the ORC JIT; prebuilt libraries are not supported and should be rejected
   before JIT materialization with a targeted diagnostic.
2. Route `import program` adapters through a JIT-visible implementation for `.cb` imports and
   the supported imported-C-source path for real C programs.
3. Keep the existing explicit `--heap-audit` restriction unless a JIT-safe implementation is
   designed. In the meantime, classify self-auditing HeapAudit fixtures separately from the
   ordinary `--run` smoke set and document the limitation.

## Verification bar

The ordinary portable `--run` smoke set must remain 30/30 on this host. Once the relevant member
is fixed, its fixture must pass under `--run` both before and after `--init-local`; the AOT suite
must remain 647/0/8 or better.

## Related

- [2026-08-13 smoke-test report](run-jit-smoke-2026-08-13.md)
- `internal/macos-build.md` - current Darwin JIT constraints
- `internal/run-jit-unwind.md` - prior JIT threading and unwind work
