# Diagnostics

cflat ships a small toolbox for chasing the bugs that ordinary `test.bat` runs do
not reliably surface: heap corruption, use-after-free, double-free, and the
timing-dependent races that only appear under load. Some are compiler flags;
others are opt-in core libraries you import and enable from your program. This
document records each tool, how to turn it on, and what class of bug it catches.

A practical rule of thumb: reach for the static scans (`--xthread-scan`) first
because they cost nothing at runtime, then `--asan` to catch a memory bug
deterministically, then `-g` for a symbolized stack on whatever still crashes,
and the fuzzer/heap-audit libraries when a bug is real but too rare to reproduce.

## Tool index

| Tool | Kind | Catches |
|------|------|---------|
| `--asan` | compile flag | use-after-free, heap/stack overflow, double-free (dynamic) |
| `-g` crash backtrace | compile flag | symbolized stack on any unhandled exception |
| `--xthread-scan N` | compile flag | struct fields shared across a thread spawn without atomic/lock (static) |
| `diagnostic/thread_fuzz.cb` | core library | timing-dependent races (perturbs scheduling, replayable by seed) |
| `diagnostic/heap_audit.cb` | core library | double-free / free-of-unknown, reported at the bad free |

## `--asan` - AddressSanitizer

Instruments the program with LLVM AddressSanitizer and links the dynamic asan
runtime. asan keeps freed memory poisoned in a quarantine, so a use-after-free or
heap overflow surfaces as a clean, located report instead of an intermittent
access violation. It is the single most effective tool here for memory bugs.

```bash
# Pair with -g so the report carries source file and line numbers.
x64/Debug/cflat.exe app.cb -i Test/library -o app.exe --asan -g
```

- Requires `-o` (asan instruments and then links its runtime; there is nothing to
  link in IR-only mode). The clang-compatible spelling `-fsanitize=address` is
  accepted as an alias.
- Instruments even at `-O0` - the whole point is to catch bugs in debug builds -
  so you do not need to also pass `-O2`.
- Tune behavior with the standard `ASAN_OPTIONS` environment variable. The set
  used to crack the threadpool UAF (see the case study below) was:

  ```bash
  ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:exitcode=99:detect_leaks=0:symbolize=1
  ```

  With `halt_on_error=1` and a distinct `exitcode`, a stress loop can count
  asan-caught faults by exit code without the report scrolling past.

**Windows caveats.** Two COFF/Windows-specific issues were fixed to make `--asan`
work at all (see the `project-asan-integration-fixes` note): the pass forces the
dynamic shadow (`asan-force-dynamic-shadow`) to match the linked
`clang_rt.asan_dynamic` runtime, and all globals are marked no-address to avoid a
COMDAT abort. One honest limitation remains: a bug that corrupts the heap or
faults during process teardown can make the asan symbolizer fault mid-report, so
those crashes come out **frameless** (the fault is caught but no stack prints).
When that happens, fall back to `-g` and the fuzzer/heap-audit tools.

## `-g` - in-process symbolized crash backtrace

Building with `-g` (DWARF/PDB debug info) links `core/diagnostic/crashdump.c`, which installs
a last-chance exception filter via a CRT dynamic initializer. On any unhandled
exception (access violation, etc.) it prints the exception kind plus a numbered,
DbgHelp-symbolized backtrace - cflat function names and source lines resolved from
the PDB next to the exe - to stderr, then ends the process quietly (no Windows
Error Reporting popup). Without it such a crash dies silently with exit code 139
and no diagnostic.

```bash
x64/Debug/cflat.exe app.cb -i Test/library -o app.exe -g
```

Output goes through Win32 `WriteFile`, not CRT stdio, because cflat's runtime
defines its own `sprintf`/`vsprintf` family that would otherwise collide at link
time. Like `--asan`, it can go frameless if the crash happens during
`ExitProcess` teardown, when stderr and the SEH machinery are already torn down.

## `--xthread-scan N` - cross-thread sharing scan

A read-only, syntactic pre-pass (no runtime cost) that finds struct instances
which escape to a spawned thread, then reports any access to a field of such a
type that is neither atomic nor lock-guarded. This is a plain compiler stdout
report with an `[xthread]` prefix - it is **not** routed to the LSP and does
**not** affect the exit code.

```bash
x64/Debug/cflat.exe app.cb -i Test/library --xthread-scan 1
```

Each finding prints once (deduped):

```
[xthread] field 'ctx.counter' (ComputeCtx) shared across spawn, not atomic/guarded
```

The level controls how aggressively escapes are recognized, trading noise for
coverage:

- **1** - address-of a local struct passed to a thread spawn (`&ctx`); lowest noise.
- **2** - level 1 plus a heap struct-pointer local handed to a spawn (pointer handoff).
- **3** - level 2 plus any struct pointer passed to *any* call, and
  default-ordering atomics no longer suppress a finding; most aggressive, most
  false positives.

This is the static counterpart to the lock-set analysis: lock-set proves a guarded
access is safe, while `--xthread-scan` flags the shared fields that have no such
discipline at all.

## `diagnostic/thread_fuzz.cb` - scheduling fuzzer

A seeded randomized thread-scheduling fuzzer. Every synchronization sched-point in
the core primitives (mutex, channel, semaphore, latch, thread) consults a
per-thread seeded RNG and sometimes yields, perturbing interleavings so a
timing-dependent bug is hit far more often than the OS scheduler hits it by luck.
A captured seed makes a crash far more likely to reproduce.

```cflat
import "diagnostic/thread_fuzz.cb";

// Call once at startup, before spawning threads.
ThreadFuzz.enable();
```

Configured by environment variables:

- `CFLAT_FUZZ_SEED` - i64 seed. If unset, one is drawn from entropy and **printed**
  so the run can be replayed.
- `CFLAT_FUZZ_PERIOD` - positive int; roughly 1 in PERIOD sched-points perturbs.
  Defaults to 8; smaller is more aggressive.

Decisions are a pure function of (seed, thread id, per-thread counter), keeping the
hot path lock-free. Honest limit: it perturbs thread *yielding* only, not core
placement or other entropy sources, so replay is "very likely" rather than
bit-for-bit. See `core/diagnostic/THREAD_FUZZ_PLAN.md` for the design and the
planned v2 (PCT) extension.

## `diagnostic/heap_audit.cb` - double-free detector

Records every allocation that flows through `operator new` (with its size) and
checks every `operator delete` against that table. A free of an already-freed
pointer is reported as a DOUBLE FREE - with the pointer and its size - and the
process is stopped at the bad free itself, deterministically, regardless of timing
or which thread did it. The size discriminates a large allocation (e.g. a channel
ring buffer) from a small handle/context allocation.

```cflat
import "diagnostic/heap_audit.cb";

// Call once at startup, before spawning threads.
HeapAudit.enable();
```

The table, lock, and failure report live in the sibling `diagnostic/heap_audit.c`
(merged in by `lld-link`, so building requires `-o`); output goes through Win32
`WriteFile` for the same link-collision reason as `crashdump.c`. Honest limit:
only allocations made *after* `enable()` that flow through `operator new`/`delete`
are tracked - pointers allocated earlier, or freed via a raw allocator path that
bypasses `operator delete`, are ignored, not flagged. It catches double-frees, not
use-after-free reads (use `--asan` for those).

## Case study: the test_threadpool UAF

These tools were built to crack an intermittent `test_threadpool` crash that
defeated cdb, procdump, TTD, and a first-generation fuzzer - every timing-based
capture suppressed the bug. `--asan` was what finally caught it: its quarantine
turned an intractable "decommit -> access violation that bypasses SEH" into a
catchable `heap-use-after-free` with allocation, free, and access stacks. The
report pinpointed a worker that published a task handle's `done` flag (releasing a
`wait()`er that then deleted the handle) and afterward read a field of the same,
now-freed handle. The fix reordered the publish to be the last touch of the handle,
under the lock that serializes it against the waiter.

The takeaway that motivated documenting these together: dynamic tools (`--asan`,
heap-audit) catch the bug when it fires, the fuzzer makes a rare bug fire often
enough to catch, and `-g` gives you a stack on whatever is left - but for the
whole class of "a worker touches a field across a thread boundary without
discipline," `--xthread-scan` can flag it before it ever runs.
