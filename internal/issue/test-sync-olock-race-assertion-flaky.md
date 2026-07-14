# `test_sync` olock "the race is real" assertion is flaky under parallel load

Created: 2026-07-14 (observed while verifying an unrelated COM fix; ~3 failures in 10 `test.bat` runs)

## Summary

`Test/test_sync.cb`'s optimistic-lock (seqlock) concurrency test asserts that the reader threads
actually *raced* the writer:

```cflat
// Without this the test is vacuous: if no read ever raced the writer, every read
// validates, torn stays 0, and the seqlock was never exercised. Measured ~33% here.
result &= Test("olock concurrent: some reads were invalidated (the race is real)",
                (int)(invalid.read() > 0), 1);
```

This is a *statistical* assertion, not a correctness one: it fails whenever the writer happens to
finish its iterations before any reader lands inside a write window, leaving `invalid == 0`. The
other 66 assertions (including "no torn reads observed", the actual correctness property) pass.

Failure mode is always the same single line:

```
olock concurrent: some reads were invalidated (the race is real) FAILED (expected 1 got 0).
Thread/Sync tests: 66/67 passed.
```

## Repro

Run `test.bat Release` repeatedly. Roughly 3 in 10 runs fail `test_sync`; the other 7 are green.
The same compiled `out\test_sync.exe` run standalone passes 67/67 every time (6/6 observed), and
also passes 4/4 while 10 CPU spinners saturate the box - so plain CPU starvation does not reproduce
it. It only shows up under `test.bat`'s ~20-way parallel test execution, which is what perturbs the
reader/writer interleaving enough for the race window to be missed entirely.

Likely aggravated by this box being heterogeneous (4 Zen5 perf cores + 6 Zen5c compact cores): under
parallel load the writer and readers can land on cores with quite different throughput.

## Root cause

The assertion depends on thread scheduling, so it is inherently probabilistic. `test.bat` runs the
whole suite in parallel, which makes the "no read ever observed a write in progress" outcome common
enough to fail the suite regularly.

## Fix direction

Do NOT delete the assertion - it is there to stop the test going vacuous, which is a real concern.
Make the race deterministic instead. Options, cheapest first:

1. Have the writer keep writing until the readers report at least one invalidation (bounded by a
   generous iteration/time cap), rather than running a fixed iteration count and hoping.
2. Widen the write window (e.g. a tiny yield/spin between the two sequence-counter bumps) so a
   concurrent reader reliably observes an odd sequence.
3. Failing both, drive the seqlock protocol directly in a single-threaded test that bumps the
   sequence counter by hand to prove invalidation is detected, and downgrade the threaded test to
   the correctness properties only (no torn reads, some reads validated).
