# A string concat in test_move's bucket-A block leaks at exit (pre-existing, masked by baseline capture)

Filed 2026-08-28, found during the expr-diags fix-issue round while attributing a test_move
failure. Pre-existing: reproduces with a master-lineage binary with none of that branch's
changes.

## Observed

Running `Test/test_move.cb` prints, at process exit, after `2076 / 2076 tests passed.`:

```
*** cflat heap-audit: LEAK ptr=... size=6 ***
  #1 _operator+_string_stringstring_+0x5c
  #2 _testBucketAExpressionOwnership_i32__+0x1ac
```

Two identical trace blocks, same pointer. Exit code is 0 and every leg passes, so the suite
stays green - the leak is invisible to test.sh.

## Why no leg catches it

`testBucketAExpressionOwnership` captures `int bucketHeapBaseline = (int)HeapAudit.reportLeaks();`
AFTER the block that performs the leaking concat (the `embeddedNul`/receiver-length block,
`string + string` producing a 6-byte buffer - "ab" + "\0cd" is the size-6 candidate). Every later
leg compares deltas against that baseline, so an allocation leaked before the capture is inside
the baseline and never fails a leg. Only the audit's at-exit report shows it.

## Root cause

Not yet diagnosed. A `string + string` concat buffer in that block is allocated and never
freed - candidate sites are the `embeddedNul = embeddedNul + "\0cd";` self-append or a temp
feeding `bucketStringValueLength(embeddedNul)` (by-value string parameter). The offset
(+0x1ac, early in the function) and size 6 ("ab\0cd" + NUL) point at the embeddedNul append.

## Fix direction

Diagnose which concat's result is orphaned (heap-audit stack plus `--out-lli` IR of a minimal
extraction; note a minimal probe of `a = a + "\0cd"` alone measured CLEAN, so the trigger needs
more of the surrounding block). Then fix the orphaning path in the compiler. Separately, the
test's baseline-capture placement hides leaks in the preceding block - after the compiler fix,
move the capture (or add a `reportLeaks` leg) above that block so the region is covered again.
