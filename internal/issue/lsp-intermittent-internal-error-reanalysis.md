# Intermittent "Internal compiler error during analysis" in the LSP bulk sweep

Created: 2026-07-13. Investigated 2026-07-14 - the crash is now NAMED but NOT FIXED.

## Status

The generic wrapper that hid this is gone: the LSP now reports the real fault, the file it
died on, and the worker stack size. Landed in `cflat/LspServer.cpp`.

**The root cause is still OPEN.** A stack-exhaustion hypothesis looked strong and was
DISPROVEN by measurement - see "Ruled out" below. Do not re-chase it.

## Symptom

`test_lsp`'s bulk sweep intermittently fails one file with `Internal compiler error during
analysis`. Roughly 10% of bulk runs. Order-dependent: a focused single-file loop does not
reproduce it.

With the new error surfacing, the fault is named:

```
[lsp] compiler crash on slot 2 analyzing '.../example/ui/01-elements/app.cb': signal 139, worker stack 256 KB
[lsp] compiler crash on slot 2 analyzing '.../example/ui/02-terminal/tui_demo.cb': signal 139, worker stack 256 KB
```

Signal 139 is SIGSEGV. Two facts fall straight out of that:

- It is **not an `app.cb` bug**. It hits `tui_demo.cb` too. The old framing ("fails on
  app.cb") was an artifact of the sweep order, not a property of the file.
- It is a **segfault**, not an assert or an `llvm_unreachable`.

## What was measured

Harness: `vscode-extension/test/lsp_bulk_test.py` driven in a loop. `CFLAT_LSP_STACK_KB`
and `CFLAT_LSP_POOL_SIZE` (both read by `LspServer.cpp`) set the worker stack and pool
size. `CFLAT_LSP_NO_CRC=1` disables `llvm::CrashRecoveryContext` so the process dies with a
real signal instead of being caught, which is how you get a core/backtrace.

```bash
#!/usr/bin/env bash
# scratch/bulk_loop.sh - repeats the LSP bulk sweep N times.
# usage: bulk_loop.sh N [pool] [stackKB]
N=${1:-10}; POOL=${2:-4}; STACK=${3:-}
mkdir -p scratch/runs
[ -n "$STACK" ] && export CFLAT_LSP_STACK_KB="$STACK"
fail=0
for i in $(seq 1 "$N"); do
    log="scratch/runs/bulk_${POOL}_${STACK:-def}_$i.log"
    python3 vscode-extension/test/lsp_bulk_test.py x64/Release/cflat --lsp-pool-size "$POOL" > "$log" 2>&1
    rc=$?
    [ $rc -ne 0 ] && { fail=$((fail+1)); tag=FAIL; } || tag=OK
    echo "run $i: $tag  $(grep -m1 'compiler crash on slot' "$log")"
done
echo "STACK=${STACK:-default} POOL=$POOL -> failures: $fail / $N"
```

Results (each row = independent bulk runs):

| worker stack | pool | runs | crashes |
|--------------|------|------|---------|
| 256 KB (forced) | 4 | 6  | **3**   |
| 512 KB (forced) | 4 | 20 | 0       |
| 16 MB (default) | 4 | 12 | **3**   |
| 16 MB (default) | 1 | 6  | **3**   |

A stack-probe (paint-and-scan high-water mark, since removed) measured **peak analysis
stack usage at ~183 KB** on the deepest example (`example/ui/09-map/map.cb`).

## Ruled out

**Stack exhaustion is NOT the root cause.** This is the important finding, because
everything about the symptom pointed at it and a fix would have looked like it worked:

- Forcing the stack to 256 KB makes it crash *much* more (3/6), which is what sold the
  hypothesis.
- But at the **16 MB** default it *still crashes* - 3/12 at pool=4, and 3/6 at pool=1.
  A 64x larger stack does not close it.
- Measured peak usage is ~183 KB, i.e. **bounded and small**. The recursion is not runaway.

So the 256 KB result is a red herring: a tight stack makes an *already-wild pointer* more
likely to land in unmapped memory, but the fault is a bad pointer, not depth. Raising the
stack only changes the odds.

**The original suspect - leaked transient state across `ResetForReanalysis` - is NOT ruled
out.** It was never actually tested. It remains the best hypothesis: it explains the
order-dependence, the pool-size sensitivity, and why a single-file loop stays clean.

## Next steps

1. Run with `CFLAT_LSP_NO_CRC=1` under a debugger / with core dumps enabled and get a real
   **backtrace**. Everything above is black-box; one backtrace likely ends this.
2. Audit `ResetForReanalysis` against every transient member of `LLVMBackend`, especially
   anything set on an error path that returns early (the `lastCallIsBonded` precedent in
   CLAUDE.md). Note the recent `long`/`alignas` work added new state to `LLVMBackend`
   (`IsPadding` on `DeclTypeAndValue`, and a **static** `longBits_`) - a static is exactly
   the shape that survives a reset.
3. The pool=1 repro (3/6) is the cheapest signal - it is more reliable than the natural
   ~10% and uses a single backend, which is the state-leak-sensitive configuration.

## What landed (in `cflat/LspServer.cpp`)

- `DescribeCrash()` - turns `CrashRecoveryContext::RetCode` into a human cause (POSIX
  signal name, or SEH exception code on Windows, including `0xC00000FD` stack overflow).
  The crash log and the LSP diagnostic now both carry it, plus the file being analyzed.
  **This is what cracked the case open and is worth keeping regardless.**
- Workers moved from `std::thread` to `llvm::thread` with an explicit 16 MB stack
  (`kDefaultAnalysisStackBytes`). Platform defaults are thin (512 KB macOS / 1 MB Windows)
  against a measured ~183 KB peak, so this is defensible **hardening** - but be clear that
  **it is not the fix** and does not close this issue.
- `CFLAT_LSP_STACK_KB` and `CFLAT_LSP_NO_CRC` diagnostic knobs.

Gate after these changes: `./test.sh Release` 181 passed / 0 failed / 8 skipped;
`./test_lsp.sh Release` 151 passed / 0 failed.
