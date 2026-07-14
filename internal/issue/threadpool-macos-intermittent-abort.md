# test_threadpool: intermittent heap-corruption abort on macOS (~1/20-40 runs)

Created: 2026-07-13 (found while sweeping `Test/*.cb` under `--heap-audit` on
macOS arm64; reproduces WITHOUT `--heap-audit` too - pre-existing, unrelated
to the heap-audit port)

## Summary

`Test/test_threadpool.cb`, compiled and run standalone (no `--heap-audit`
needed to reproduce), intermittently aborts with SIGABRT instead of printing
its usual `ThreadPool tests: 23/23 passed.` and exiting 0. Measured rate:
about 1 in 20-30 runs (3 aborts / 60 plain runs in this session; separately,
2 aborts / 55 runs WITH `--heap-audit` and 1 abort / 55 runs WITHOUT it were
observed earlier in the same sweep - consistent rates, so `--heap-audit`
itself is not implicated). When it does not abort, all 23/23 subtests pass.

## Repro

```bash
x64/Release/cflat Test/test_threadpool.cb -i Test/library -o /tmp/test_threadpool
cd /tmp
n=0; aborts=0
while [ $n -lt 60 ]; do
  n=$((n+1))
  ./test_threadpool > /dev/null 2>&1 || aborts=$((aborts+1))
done
echo "$aborts / $n aborted"
```

Typical result on Apple Silicon (arm64, macOS): 2-3 aborts per 60 runs
(exit code 134). No output is captured on the abort runs - stdout is fully
buffered (redirected to a file, not a tty) and `abort()` does not flush it,
so the crash looks silent unless caught live.

## Crash signature (caught live under lldb; three independent catches, all identical)

```
malloc: Heap corruption detected, free list is damaged at 0x...
*** Incorrect guard value: ...
```

Full backtrace of the crashing thread (thread #1, main thread), captured via:

```bash
lldb -b -s /dev/stdin -- ./test_threadpool <<'EOF'
run
thread backtrace all
EOF
```//run in a loop (see above) until it catches one; took ~30-70 lldb-driven runs.

```
* thread #1, stop reason = signal SIGABRT
    frame #0: libsystem_kernel.dylib`__pthread_kill
    frame #1: libsystem_pthread.dylib`pthread_kill
    frame #2: libsystem_c.dylib`abort
    frame #3: libsystem_malloc.dylib`malloc_vreport
    frame #4: libsystem_malloc.dylib`malloc_zone_error
    frame #5: libsystem_malloc.dylib`nanov2_guard_corruption_detected
    frame #6: libsystem_malloc.dylib`nanov2_allocate_outlined
    frame #7: libsystem_malloc.dylib`nanov2_malloc_type_zero_on_alloc
    frame #8: test_threadpool`_os.thread_create_U8Ptr_cfuncptr_void_voidPtrU8Ptr_
    frame #9: test_threadpool`_start_bool_ThreadPtrcfuncptr_int_voidPtrMU8PtrMint_   (Thread.start)
    frame #10: test_threadpool`_resize_void_ThreadPoolPtrint_                        (ThreadPool.resize)
    frame #11: test_threadpool`_testResizeGrowShrinkGrow_bool__
    frame #12: test_threadpool`main
```

Other threads (#2, #3) are idle pool workers parked in `semaphore_wait_trap`
(`acquire()` on the pool's ready-semaphore) - not implicated directly, but
their existence confirms the crash happens while worker threads from a PRIOR
`resize()`/`init()` call are still alive/running concurrently with the next
`resize()` call.

Nano-zone guard corruption is detected on the NEXT allocation after the
actual out-of-bounds write/double-free, not necessarily at the exact byte
that was corrupted - so this backtrace shows where the corruption was
DETECTED (a plain `new`/malloc inside `os.thread_create`, unrelated in
content to whatever got corrupted), not necessarily where the bad write
happened. Root cause is diagnosed to the crash SIGNATURE and TEST (heap
corruption inside `ThreadPool.resize()`'s grow path, always in
`testResizeGrowShrinkGrow`), but the exact corrupting write was not isolated
within the time-boxed investigation.

## Root cause (hypothesis, not fully confirmed)

`ThreadPool.resize()` (`cflat/core/threadpool.cb:622`) documents itself as
"NOT thread-safe against concurrent submit(), then(), drain(), or shutdown():
grow reallocates the per-worker arrays that those paths read unlocked. The
caller must quiesce the pool for the duration of resize()." The grow path
(`numa.cb`... rather `threadpool.cb:627-662`) does, when growing past
capacity:

```cflat
Thread*      nw = new Thread[newCount];
u32*         ni = new u32[newCount];
stop_source* ns = new stop_source[newCount];
... copy old arrays into new ...
delete[_] _workers;
delete[_] _workerIds;
delete[_] _workerStops;
_workers = nw; _workerIds = ni; _workerStops = ns;
_ctx.workerIds = ni; _ctx.workerStops = ns;
```

Live worker threads (`__threadpool_worker`, `threadpool.cb:855`) write their
own tid into `wctx->workerIds[slot]` at start (`:870`) and clear it at exit
(`:892`) via the SHARED `_ctx` pointer (live dereference of the current
`workerIds` field, not a value captured once) - so any worker starting or
exiting in the exact window between `delete[_] _workerIds;` and
`_ctx.workerIds = ni;` would write through a dangling pointer into freed
memory, which matches a "free list is damaged" signature well. However,
`testResizeGrowShrinkGrow()` calls `resize()` sequentially with no other pool
call in flight (satisfying the documented contract on its face), and the
only "grow past capacity" reallocation happens on the very FIRST `resize(4)`
call while the pool's original 2 workers (from `init(2)`) are alive and
idle - so the precise interleaving that would trigger a live worker's
startup/exit write during the array swap is not obviously present in this
test. This suggests either (a) a narrower race than described above (e.g. in
the shrink path's `try_join(0)` polling loop, or in `stop_source`/semaphore
teardown), or (b) the race is present but requires the OS scheduler to
preempt at a very specific point, which is consistent with the low (~3-5%)
observed rate.

## Fix direction (for whoever picks this up)

1. Reproduce under a stricter memory checker (ASan via `--asan`, or
   `MallocStackLogging=1` + `MallocScribble=1` + a crash, then
   `malloc_history` on the corrupted address) to get the actual corrupting
   write's stack, not just the detection site.
2. Once the exact write is identified, the likely fix is adding a lock
   around `ThreadPool.resize()`'s array swap (`_workers`/`_workerIds`/
   `_workerStops`/`_ctx.workerIds`/`_ctx.workerStops`) so a concurrently
   starting/exiting worker cannot observe a torn/freed pointer - or
   documenting an explicit barrier (all workers must be parked, confirmed via
   an OS-level wait, before the arrays are freed) beyond what
   `try_join(0)` in the shrink path already does.
3. Add a regression case in `Test/test_threadpool.cb` (extend
   `testResizeGrowShrinkGrow` or add stress iterations) once the race is
   fixed, run in a loop (50-100x) to confirm the abort no longer reproduces.
