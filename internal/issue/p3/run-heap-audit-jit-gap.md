# `--run` cannot resolve the C-backed HeapAudit symbols

Filed 2026-08-31, carried over from the `--run` JIT smoke report in the retired
`internal/issue/queue/` bucket directory (report dated 2026-08-13). This is the LAST remaining
item of that report's q19 group; everything else in it was implemented.

## Summary

Fixtures whose self-auditing code calls the C-backed HeapAudit functions cannot run under
`--run`. The in-process JIT resolves imported CFlat and imported C-source symbols, but not the
C-backed diagnostic module that HeapAudit lives in.

Affected fixtures: `test_collection_leaks`, `test_list_ownership`, `test_reflect`.

## Repro

```bash
x64/Release/cflat Test/test_collection_leaks.cb -i Test/library --run
```

Fails on unresolved HeapAudit symbols. The same fixtures pass under the ordinary AOT path.

## Root cause

The explicit `--heap-audit` option already rejects `--run` with a clear diagnostic, because it
requires a linked C diagnostic object. These fixtures reach the same object through a
SOURCE-LEVEL import rather than the flag, so they bypass that diagnostic and fail later at JIT
symbol resolution instead.

## Disposition

**Deliberately lower priority.** The 2026-08-13 triage ordered this last of four items and ruled
that AOT-only leak validation is an acceptable near-term disposition, since `--heap-audit` is
already AOT-only by design. The remaining choice is between implementing HeapAudit parity for the
JIT and giving the source-level import the same targeted pre-JIT rejection the flag already gets.

## Context from the smoke report

The in-process JIT is otherwise viable for the portable suite: 30/30 compatible fixtures passed
(ownership, generics, SIMD/HPC, synchronization, process, threadpool), plus `test_allocators`,
`test_stream`, `test_threadpool` via special entry points, and the argv probe. `test.sh --run` is
an opt-in smoke mode and passed 35/35 selected checks. Prebuilt C libraries are unsupported by
design under `--run` and are now rejected before JIT materialization with a targeted diagnostic.
