# HeapAudit-backed fixtures fail under `--run`

## What

Fixtures that import `diagnostic/heap_audit.cb` directly fail under `--run` because that module
imports a C implementation. The affected smoke fixtures are:

- `Test/test_collection_leaks.cb`
- `Test/test_list_ownership.cb`
- `Test/test_reflect.cb`

## Repro

```text
x64/Release/cflat Test/test_list_ownership.cb -i Test/library --run --nologo
```

Observed:

```text
JIT session error: Symbols not found: [ _cflat_heap_audit_alloc,
_cflat_heap_audit_report_leaks, _cflat_heap_audit_free ]
```

The explicit `--heap-audit` option already rejects `--run` because it requires a linked C
diagnostic object. These fixtures bypass that option but still require the same native symbols.

## Triage

Last in q19 and not a prerequisite for ordinary `--run` correctness. Keep these tests AOT-only in
the near term unless the order-1 ORC object-loading work makes a JIT-safe audit backend natural.

The source-C object-loading and imported-program work from q19 landed on 2026-08-13. This remains
the only unresolved item from that bucket.

## Root cause

`core/diagnostic/heap_audit.cb` declares the C functions from `heap_audit.c`; `JitRun` does not
compile or load that C object into ORC. The unresolved symbols appear only when the JIT tries to
materialize `main`.

## Fix direction

This is a lower-priority parity gap, not evidence that the ordinary `--run` path is broken. Either
provide a JIT-safe HeapAudit implementation, or make source-level HeapAudit imports explicitly
incompatible with `--run` and report that before materialization. Until then, keep these fixtures
in an AOT-only leak-validation group rather than counting them as ordinary `--run` smoke tests.

## Related

- `internal/issue/queue/run-jit-smoke-2026-08-13.md`
- `cflat/core/diagnostic/heap_audit.cb`
- `cflat/core/diagnostic/heap_audit.c`
- `cflat/main.cpp` (`--heap-audit` validation)
