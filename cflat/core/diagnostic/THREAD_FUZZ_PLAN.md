# Thread Fuzzing - Design Plan (core/diagnostic)

Status: PROPOSED (not yet implemented). This file lives under `core/diagnostic/`
on purpose: thread fuzzing is an opt-in debugging tool, not an everyday library.
Nothing here is linked or active unless a program explicitly imports the module
and enables it via an environment variable.

## 1. Goal and scope

Make timing-dependent concurrency bugs reproducible on demand. Today a race like
the ThreadPool teardown crash surfaces at ~0.5% under heavy oversubscription and
gives you nothing but exit code 139 - impossible to study, impossible to replay.

This tool perturbs thread scheduling from a seed so that:
- a bad interleaving is hit far more often than the OS scheduler hits it by luck,
- a captured seed reproduces the same run, which pairs with the new `-g`
  in-process symbolized backtrace to turn "it crashed somewhere" into a stack.

### In scope
Interleaving races expressed at synchronization boundaries:
- `mutex` acquire / release (and the `lock` statement, which lowers to them)
- `channel` try_push / try_pop, `semaphore` acquire / release, `latch`
- `Thread.start` / `join`, and condition-variable waits (`sleep_cv`)

### Explicitly NOT in scope (by decision)
- Atomic load/store/cas/fence are NOT instrumented. The lock/guard mechanism plus
  the compile-time lock-set analysis already constrain shared-state access, so the
  lock-boundary sched-points cover the interesting interleavings without paying for
  atomics. (Atomic-level and raw-memory instrumentation would require a compiler
  pass; see "Future" - deferred.)
- Memory-model races (relaxed/release/acquire reordering, torn reads) are NOT a
  target. A cooperative scheduler serializes execution and cannot reproduce
  hardware reordering. That is a separate tool (a memory-model fuzzer).
- This is a triggering tool, not a detector. It does not prove race-freedom (that
  is lock-set analysis) and does not flag benign data races (that would be a
  TSan-style detector). It makes real interleaving bugs happen reliably.

## 2. Design principle: library-only, hook-based, near-zero everyday cost

No compiler changes for v1. The mechanism is a single global hook plus one guarded
call at each sched-point:

- `runtime.cb` gains one global: `function<void(int)> __fuzz_hook = nullptr;`
  (default null -> fuzzing off -> the only everyday cost is one predictable
  null-pointer compare per sync operation).
- Each sync primitive adds, at its sched-point(s):
  `if (__fuzz_hook != nullptr) { __fuzz_hook(SITE_ID); }`
- The actual scheduler lives entirely in `core/diagnostic/thread_fuzz.cb`. Importing
  that module and calling `ThreadFuzz.enable()` installs `__fuzz_hook`; not importing
  it means the hook stays null and the module is never linked.

This keeps everyday primitives clean (no dependency from `core/mutex.cb` onto the
diagnostic module - just a shared global declared in `runtime.cb`) and makes the
tool obviously opt-in.

## 3. The "few small changes" (v1)

1. `runtime.cb`: declare `function<void(int)> __fuzz_hook = nullptr;` and a small
   set of site-id constants (or just pass an int literal per call site).
2. `core/mutex.cb`: guarded hook call at the top of `acquire()` and `release()`.
   (The `lock` statement lowers to these, so it is covered for free.)
3. `core/channel.cb`: guarded hook call in `try_push` / `try_pop` (before the CAS
   region begins, not inside it).
4. `core/semaphore.cb`, `core/latch.cb`: guarded hook call in acquire/release/wait.
5. `core/thread.cb`: guarded hook call at child-thread entry (in the trampoline,
   so the new thread registers itself) and around `join()`.
6. New file `core/diagnostic/thread_fuzz.cb`: the scheduler (sections 4-5).
7. `cflat.vcxproj`: deploy `core/diagnostic/thread_fuzz.cb` (DeploymentContent),
   mirroring an existing `core/*.cb` entry.

That is the whole surface. Everything else is contained in the diagnostic module.

## 4. v1 (recommended first step): seeded randomized yielding

Smallest thing that is genuinely useful. The hook, on each call, consults a
per-thread seeded RNG and sometimes yields:

```
// inside thread_fuzz.cb (sketch)
void __fuzz_on_syncpoint(int site) {
    FuzzState* s = __fuzz_tls();          // per-thread state, lazily created
    s->counter = s->counter + 1;
    u64 r = splitmix64(&s->rng);
    if ((r % g_fuzz_period) == 0) {
        // randomly chosen perturbation: cheap yield or a short timed sleep
        if ((r >> 8) & 1) { SwitchToThread(); }
        else              { sleep(0); }
    }
}
```

Key points:
- Per-thread RNG seeded from `global_seed XOR thread_id` (plus a per-thread call
  counter). Draws need no shared state and no locking, so the hot path stays cheap
  and each thread's decisions are a pure function of (seed, tid, local counter) -
  which also makes replay far more stable than a shared, racy RNG.
- `global_seed` and `g_fuzz_period` come from the environment
  (`CFLAT_FUZZ_SEED`, `CFLAT_FUZZ_PERIOD`) read via `getenv` at `enable()`.
  If `CFLAT_FUZZ_SEED` is unset, draw one from entropy and PRINT it, so any crash
  is replayable after the fact.

Pros: ~50 lines, pure library, immediately raises the hit rate for interleaving
bugs (the teardown crash is the canary - see section 7). Cons: no formal coverage
guarantee; replay is "very likely" not "bit-for-bit" (yields are seeded, but the
OS still places threads on cores).

## 5. v2 (follow-up): PCT controlled scheduler

When v1's randomization is not reliable enough, upgrade the same hook to a real
cooperative scheduler implementing PCT (Probabilistic Concurrency Testing):

- Each participating thread registers on start (via the thread.cb entry hook) and
  gets a per-thread gate (auto-reset event / binary semaphore) and a random
  priority from the seed.
- `__fuzz_hook` becomes `fuzz_yield`: the calling thread marks itself runnable,
  the scheduler grants the baton to the highest-priority runnable thread by
  signaling its gate, and the caller blocks on its own gate until signaled. Exactly
  one thread runs between sched-points.
- PCT picks `d-1` random priority-change points across the run; reaching one lowers
  the running thread's priority, forcing a switch. This gives the PCT guarantee: a
  specific depth-`d` bug is hit with probability >= 1 / (n * k^(d-1)) per seed
  (n threads, k sched-points). Shallow bugs (d = 1..2, the common case) are caught
  with good odds, and each seed is independent and replayable.

v2 is more code (per-thread gates, registration, the baton scheduler) but reuses
the identical hook sites from v1 - no new instrumentation. It also gives true
deterministic interleaving replay (the OS no longer chooses; the scheduler does).

## 6. Determinism and replay - honest limits

The scheduler controls thread interleaving only. Other entropy still varies run to
run: `TimePoint.now()` branches, `random.cb` draws in user code, ASLR addresses,
dictionary iteration order. For reproducing a race the interleaving is what matters,
so v1/v2 are effective; for bit-for-bit identical replay you would also have to pin
those sources. Document this so CI does not over-promise "exact replay."

## 7. Validation plan

Use the ThreadPool teardown crash as the built-in canary (it is a real ~0.5%
interleaving bug as of this investigation):
1. Baseline: measure its crash rate with fuzzing off (~0.5% at 24-wide).
2. v1 on: expect a large lift in crash rate at equal or lower oversubscription;
   capture the seeds that crash.
3. Replay a captured seed under `-g`; confirm the symbolized backtrace points at
   the same teardown frame each time. That backtrace is what finally identifies
   the root cause (which the pool-lifecycle "fix" did NOT address - see the
   investigation notes).
4. Confirm fuzzing OFF (no import / hook null) leaves `test.bat` timings and
   results unchanged (the only delta is one null compare per sync op).

## 8. Rollout

- Opt-in: `import "diagnostic/thread_fuzz.cb"; ThreadFuzz.enable();` plus
  `CFLAT_FUZZ_SEED=...`. Never on in shipped builds.
- Ships under `core/diagnostic/` to signal "debugging tool, not everyday library."
- Pairs with the `-g` crash backtrace: fuzzer makes the crash deterministic, the
  backtrace says where it is.

## 9. Deferred / future (not now)

- Compiler-injected sched-points for atomics and raw escaped-memory accesses (the
  TSan-style IR pass). Deferred per decision in section 1: lock/guard + lock-set
  analysis already cover the shared-state interleavings we care about, and keeping
  v1 library-only is the priority.
- Memory-model fuzzing (separate tool).
- A `--thread-fuzz` compiler flag that auto-imports the module and emits the hook
  calls with zero everyday cost (removes the null-compare). Only if the always-on
  null check ever shows up in a profile.
