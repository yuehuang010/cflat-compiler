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
| `--heap-audit` | compile flag | applies `diagnostic/heap_audit.cb` with no source edits (leaks, report-only) |
| `diagnostic/thread_fuzz.cb` | core library | timing-dependent races (perturbs scheduling, replayable by seed) |
| `diagnostic/heap_audit.cb` | core library | leaks (still-live allocations at a quiescent point) |

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

```cpp
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

## `diagnostic/heap_audit.cb` - leak detector

Records every allocation that flows through `operator new` (with its size) and flips
the matching record to freed on every `operator delete`. At a quiescent point it
reports every allocation still live - a LEAK, with the pointer and its size - to
stderr. The report is **advisory and non-fatal**: it does not abort the process. It
does **not** detect double frees: `operator new`/`delete` share the CRT heap with raw
`malloc`/`free`, so a freed address reused by a raw allocator and later operator-deleted
is indistinguishable from a real double free. Use `--asan`, which proves a real
double-free/use-after-free deterministically.

```cpp
import "diagnostic/heap_audit.cb";

// Call once at startup, before spawning threads.
HeapAudit.enable();
```

To audit a program **without editing it**, pass `--heap-audit` (requires `-o`): the
compiler auto-imports this module, calls `enable()` at the top of `main`, and reports
still-live allocations before every `return`. Leak reports are report-only (printed, exit code
unchanged); a program that already calls `enable()` itself is left uninstrumented. See
[`doc/CLI.md`](CLI.md#heap-audit) for the flag's full behavior. Call
`reportLeaks()` by hand instead when you need the live count at a specific quiescent point
rather than at process exit.

Each `LEAK` line is followed by the **allocation-site backtrace** - the call stack captured
when that block was allocated, symbolized with DbgHelp the same way the `-g` crash handler is.
Compile with `-g` to get cflat function names and `file:line` for each frame; without it the
frames still print as `module+address`. The first one or two frames are the allocator plumbing
(`operator new` and the audit hook); the first frame in your own code is the leaking `new`.

The table, lock, and leak report live in the sibling `diagnostic/heap_audit.c`
(merged in by `lld-link`, so building requires `-o`); output goes through Win32
`WriteFile` for the same link-collision reason as `crashdump.c`. Honest limit:
only allocations made *after* `enable()` that flow through `operator new`/`delete`
are tracked - pointers allocated earlier, or freed via a raw allocator path that
bypasses `operator delete`, are ignored, not flagged. It catches leaks, not
double-frees or use-after-free reads (use `--asan` for those).

## Diagnostic localization - authoring and translation

Compiler diagnostics are localized. The canonical English template stays in the
C++ source; translations live in JSON catalogs under `cflat/locales/`, deployed
next to the compiler at build time.

### Authoring a diagnostic

Call `LogErrorMessage` with the unformatted English template and ordered
arguments. Do not pre-format the message, and do not invent an error code or a
named symbol - the catalog key is derived from the template text:

```cpp
LogErrorMessage("use of moved variable '{}'", { moved });
```

Rules that keep a call site translatable:

- The template must be a **string literal** (adjacent literals across lines are
  fine). A template built with `std::format` or concatenated at runtime cannot be
  keyed, and the extractor reports it.
- Every `{}` needs exactly one argument, in order. A translation may reorder them
  because catalog values use numbered placeholders (`{0}`, `{1}`).
- Put dynamic text - identifiers, type names, paths - in arguments, never in the
  template. Two templates that differ only by a spliced-in name are two keys that
  translators must handle twice.
- Genuinely preformatted or third-party text (linker, LLVM, clang output) goes
  through `LogRawError`, which is deliberately not localized.

### Catalog keys

The key is derived, not written: lowercase the template, replace each `{}` with
`arg0`, `arg1`, ..., drop every non-alphanumeric character, and compact anything
over 40 characters to `<first 20>...<last 20><16-hex FNV-1a of the full key>`.
`NormalizeKey` in `cflat/DiagnosticLocalization.cpp` is the single source of
truth. Changing the English wording changes the key, which orphans the old
translations - expected, and reported by the extractor as a stale entry.

### Catalogs

`cflat/locales/<name>.json` holds `locale` plus a `messages` map of key to
translated template. `en.json` is the default display catalog and must
cover every key; the source template is only the fallback. `en-pseudo.json` is
the migration catalog: it carries the English source templates plus
`argumentExamples` (real values observed at runtime), and the extractor also
writes `argumentNames` and `sites` there as translator context.

To add a language, copy `en.json`, set `locale` to the file's basename,
and translate the values. Name the file by BCP-47 tag: a plain primary subtag
where that suffices (`de`, `fr`, `it`, `ja`, `ko`, `ru`), and by script for
Chinese (`zh-Hans`, `zh-Hant`) because the client sends regions (`zh-CN`,
`zh-TW`) that the compiler maps onto scripts.

### Keeping catalogs complete

Two producers write `en-pseudo.json`, and the order matters:

```bash
x64/Release/cflat --locale pseudo --update-locale en-pseudo --locale-dir cflat/locales \
    --check Test/errors/err_*.cb -i Test/library
python3 utilities/extract_diagnostics.py --report
```

The compiler pass is the only source of real argument values, but it sees only
diagnostics a test actually provokes. The extractor statically scans every
`LogErrorMessage` call site, so it supplies the complete key set - and carries
the observed examples forward. Run the compiler first, the extractor last.

`--report` also prints the remaining unmigrated `LogError(` call sites and every
key with no example, which is the missing-negative-test backlog. `--strict` exits
non-zero when a call site cannot be extracted cleanly.

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
