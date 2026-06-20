# Threading, Memory & Synchronization

## Table of Contents

- [Memory Management](#memory-management)
  - [`new` and `delete`](#new-and-delete)
  - [Custom Allocators](#custom-allocators-operator-new--operator-delete)
- [Concurrency](#concurrency)
  - [Threads](#threads-corethread.cb)
  - [Cancellation](#cancellation-corestop_tokencb)
  - [Mutex & `lock` Statement](#mutex--lock-statement-coremutexcb)
  - [Atomic](#atomic-coreatomic.cb)
  - [Other Synchronization Primitives](#other-synchronization-primitives)
  - [Barrier](#barrier-corebarriercb)
  - [Thread Pool](#thread-pool-corethreadpoolcb)
- [Compile-Time Lock-Set Analysis](#compile-time-lock-set-analysis)
  - [Guarded Field Groups](#guarded-field-groups)
  - [Lock Statement](#lock-statement)
  - [Function Clause (call-site contract)](#function-clause-call-site-contract)
  - [Positional Method Groups](#positional-method-groups)
  - [Reader-Writer Locks](#reader-writer-locks)
  - [Atomic Memory-Ordering Guards](#atomic-memory-ordering-guards)

**Related:** [HPC & SIMD](HPC.md) (`vectorize`, `simd<T,N>`) | [Language & Core Library Features](LANGUAGE.md)

---

## Memory Management

### `new` and `delete`

```c
class Node { int value = 0; };

Node* n   = new Node();
delete n;

int* arr  = new int[5];
delete[] arr;
```

**`delete[n]`** - call destructors on exactly `n` elements, then free. Use when the array was allocated without a cookie (e.g. raw `malloc`/`operator new`) and you know the count:

```c
Node* nodes = new Node[5];
delete[5] nodes;    // calls ~Node() on each of the 5 elements, then frees
```

**`delete[_]`** - free the backing buffer *without* calling any destructors. Use after you have already destroyed elements manually (e.g. with `.~()`):

```c
Node* buf = new Node[3];
buf[0].~();   // manual in-place destruction
buf[1].~();
buf[2].~();
delete[_] buf;   // free only; destructors already ran
```

Summary of array-delete forms:

| Form | Destructor calls | Count source |
|------|-----------------|--------------|
| `delete[] p` | all elements | hidden cookie |
| `delete[n] p` | exactly `n` elements | explicit literal/variable |
| `delete[_] p` | none | - |


### Custom Allocators (`operator new` / `operator delete`)

Override the global allocator by defining these operators:

```c
extern void* malloc(i64 size);
extern void  free(void* ptr);

void* operator new(long long size) { return malloc(size); }
void  operator delete(void* ptr)   { free(ptr); }
```

---

## Concurrency

### Threads (`core/thread.cb`)

```c
import "thread.cb";

int worker(void* ctx)
{
    int* p = (int*)ctx;
    *p = 99;
    return 0;
}

int value = 0;
Thread t;
t.start(worker, &value);
t.join();
// value == 99
```

Pass lambdas directly to `start`:

```c
Thread t;
t.start((void* ctx) => { return 42; }, nullptr);
i32 code = t.join();   // 42
```

**Thread methods:**

| Method | Returns | Notes |
|--------|---------|-------|
| `start(fn, ctx, fpConfig = 0)` | `bool` | borrow overload - the caller keeps ownership of `ctx` and must keep it alive until `join()`; the thread's `fn` must not free it. Returns `false` if the OS thread could not be created. Optional `fpConfig` arms the new thread's FP environment (see [Per-thread floating-point environment](#per-thread-floating-point-environment-corethreadcb)); `0` = leave it at the default. |
| `start(fn, move ctx, fpConfig = 0)` | `bool` | move overload - ownership of `ctx` transfers to the thread (the caller's pointer is auto-nulled), and `fn` (typed `int(move void*)`) frees it. Same optional `fpConfig` as the borrow overload. |
| `join()` | `i32` | block until the thread exits; returns its exit code and releases the handle. |
| `try_join(i32 ms)` | `bool` | block up to `ms`; returns `true` and cleans up if the thread exited, `false` on timeout (handle left intact). |
| `setAffinity(u64 mask)` | - | pin the thread to a CPU set; `mask` is a bitmask of allowed cores (`1` = core 0, `2` = core 1, `3` = cores 0+1, ...). Call after `start()`, before `join()`. |
| `terminate()` | - | forcibly kill the thread; leaks anything it held. Use only when cooperative cancellation (`stop_token`) is impossible. |

**Affinity** matters for performance work: pinning a producer and consumer (or per-core HPC workers) to separate physical cores avoids OS scheduling jitter and cross-core cache bouncing.

```c
Thread t;
t.start(worker, &ctx);
t.setAffinity((u64)1 << 2);   // pin to core 2
// ...
t.join();
```

**Logical processor count** - `hardware_concurrency()` (a free function in `thread.cb`) returns the number of logical processors, for sizing a worker count or a `ThreadPool`:

```c
int cores = hardware_concurrency();   // e.g. 20; 0 if it cannot be determined
```

**Per-thread allocator inheritance.** `start()` propagates the parent's active allocator to the child: it calls `spawn_child()` on the installed allocator, which lets each allocator choose its policy - `MallocAllocator` shares itself (the child uses the same global heap), while a per-thread allocator like `BucketAllocator` hands the child a fresh instance registered for cleanup. This is what makes `new`/`delete` safe on worker threads spawned inside a `program` scope; without it a child would start with no allocator and fall through to raw CRT `malloc`/`free`, corrupting a vmem-backed allocator's pages. You don't call anything to opt in - it happens on every `start()`. (See [Memory Management](#memory-management) for the allocator interface itself.)

### Per-thread floating-point environment (`core/thread.cb`)

The SSE control word (MXCSR) is **per-thread** hardware state controlling how
floating-point exceptions and denormals are handled. CFlat leaves every thread
at the C runtime default: FP exceptions are masked (a `0.0/0.0` or overflow
yields a quiet `NaN`/`Inf` that propagates silently) and denormals are computed
at full IEEE precision (which can be 10-100x slower in a hot loop).

A thread can opt into a different environment **at creation time** through a
single `int` knob, `fpConfig`: trap bits OR'd with an optional flush-to-zero
flag. The default `0` means "leave the control word untouched" - zero behavior
change. The setting is **per-thread and not inherited**: it is applied on the
spawned thread before its body runs and never touches the main thread or any
linked C library. The main thread stays at the C default, which is why HPC work
that wants trapping or flush-to-zero runs on spawned threads.

**Constants** (file-scope, in `thread.cb`):

| Constant | Meaning |
|----------|---------|
| `FP_TRAP_INVALID` | Trap on an invalid operation (e.g. `0.0/0.0`, `sqrt` of a negative) - faults at the producing instruction instead of yielding a quiet `NaN`. |
| `FP_TRAP_DIVZERO` | Trap on division by zero. |
| `FP_TRAP_OVERFLOW` | Trap on overflow to `Inf`. |
| `FP_TRAP_DEFAULT` | `FP_TRAP_INVALID | FP_TRAP_DIVZERO | FP_TRAP_OVERFLOW` - the usual "catch NaN/Inf at the source" set. |
| `FP_FLUSH_DENORMALS` | Flush-to-zero / denormals-are-zero: denormal inputs and results become `0.0`. An HPC throughput knob - trades a small amount of precision near zero for avoiding the denormal slow path. |

OR the flags together: `FP_TRAP_DEFAULT | FP_FLUSH_DENORMALS` both traps and flushes.

**Arming a thread at creation** - pass the config as the optional third argument
to `start`:

```c
import "thread.cb";

// Trap NaN/Inf at the source on this worker; the main thread is unaffected.
Thread t;
t.start(solverStep, &ctx, FP_TRAP_DEFAULT);
t.join();
```

**Arming from inside a running thread** - the thin wrappers `fp_enable_traps(mask)`
and `fp_disable_traps()` change the current thread's environment directly:

```c
fp_enable_traps(FP_TRAP_DEFAULT);   // from here on, NaN/Inf faults on this thread
// ... numerically sensitive region ...
fp_disable_traps();                 // back to masked (quiet-NaN) behavior
```

> Trapping is a **diagnostic** tool: an armed thread that produces a `NaN`/`Inf`
> raises a hardware FP exception (SIGFPE) at the instruction that produced it,
> so the fault points at the source rather than at the distant place the garbage
> first becomes visible. Use it while hunting a blow-up; ship with it off (or use
> `FP_FLUSH_DENORMALS` alone, which never faults) once the kernel is trusted.

**Thread pools and parallel helpers** carry the same knob:

- `ThreadPool.init(workerCount, pinMask, fpConfig)` - every worker the pool spawns
  (including workers added by a later `resize`) is armed with `fpConfig`. The third
  argument is optional and defaults to `0`.
- Raw `parallel_for_n(n, workers, body, pin, fpConfig)` and
  `parallel_reduce<T>(n, workers, identity, partial, combine, pin, fpConfig)` arm
  each worker thread they spawn. The `_pool` variants take no `fpConfig` - they run
  on a `ThreadPool`'s workers and inherit whatever that pool was `init`'d with.

```c
import "threadpool.cb";
ThreadPool pool;
pool.init(hardware_concurrency(), cpu_mask_lowest(8), FP_FLUSH_DENORMALS);
// every pooled worker flushes denormals
```

**`program` construct** - a program runs its `main` on a dedicated thread and
exposes a user-settable `_fpConfig` field (mirroring `_allocator`). Set it before
`run()` to arm the program thread:

```c
MyProg p;
p._fpConfig = FP_TRAP_DEFAULT;   // arm the program thread (default 0 = no-op)
p.run(args);
```

### Cancellation (`core/stop_token.cb`)

```c
import "stop_token.cb";

StopSource src;
StopToken token = src.token();

// In another thread: check token.isCancellationRequested()
src.cancel();
```

### Mutex & `lock` Statement (`core/mutex.cb`)

```c
import "mutex.cb";

Mutex m;
m.acquire();
// critical section
m.release();
```

Use the `lock` statement for scoped acquisition - the lock is released automatically at the end of the block:

```c
lock (m)
{
    // automatically released at end of block
}
```

Multiple locks can be acquired together in one statement:

```c
lock (a.mtx, b.mtx)
{
    // both locks held here; released together at end of block
}
```

### Atomic (`core/atomic.cb`)

`Atomic<T>` (from `mutex.cb`) is the high-level wrapper for a single integer value with seq-cst load/store/CAS:

```c
import "mutex.cb";

Atomic<int> counter;
counter.store(0);
counter.fetchAdd(1);
int v = counter.load();
```

`atomic.cb` also provides low-level value-typed atomics - `atomic__i32`, `atomic__i64`, and `atomic_flag` - used when you need release/acquire ordering or want to guard associated fields (see [Atomic Memory-Ordering Guards](#atomic-memory-ordering-guards)):

```c
import "atomic.cb";

atomic__i32 seq = default;
seq.store(1);
int v = seq.load();
seq.fetchAdd(1);
```

> `atomic_flag` is a spinlock primitive (`test_and_set` / `clear`); it has no `load()`. Use `atomic__i32` when you need to poll a flag value.

### Other Synchronization Primitives

- `core/semaphore.cb` - `Semaphore` with `acquire`/`release`
- `core/latch.cb` - `Latch` countdown; `countDown`, `wait` (one-shot - see [Barrier](#barrier-corebarriercb) for the reusable equivalent)
- `core/barrier.cb` - `barrier` / `spin_barrier` reusable rendezvous; `arrive_and_wait`
- `core/rwlock.cb` - `RwLock`; `acquireRead`/`releaseRead`, `acquireWrite`/`releaseWrite`
- `core/channel.cb` - `channel<T>` blocking MPMC queue; `send`, `recv`, `tryRecv`
- `core/spsc_queue.cb` - `spsc_queue<T>` wait-free single-producer/single-consumer ring

```c
import "channel.cb";

channel<int> ch;
ch.init(1024);
ch.send(42);
int v = ch.receive();   // 42
```

**Piping (`operator>>`)** - `src >> dst` spawns a forwarder that drains every value from `src` into `dst`. It uses borrow semantics: when `src`'s last producer closes (`close_producer()`), the forwarder drains the remaining items and closes `dst`. Pipes can be chained, and EOF propagates through every stage:

```c
channel<int>* src = new channel<int>();
channel<int>* mid = new channel<int>();
channel<int>* dst = new channel<int>();
src.init(64); mid.init(64); dst.init(64);

src >> mid;             // forwarder: src -> mid
mid >> dst;             // forwarder: mid -> dst

src.add_producer();
int i = 0;
while (i < 50) { src.push(i); i = i + 1; }
src.close_producer();   // EOF flows src -> mid -> dst, closing each

int v = 0;
while (dst.receive(&v)) { /* receives all 50 in order */ }
```

Both channels must have the same element type `T`; piping `channel<int>` into `channel<float>` is a compile error (no matching `operator>>` overload).

### Barrier (`core/barrier.cb`)

A **barrier** makes a fixed group of N threads rendezvous: each calls `arrive_and_wait()`, which blocks until all N have arrived, then releases them together *and resets itself* for the next round. Unlike `latch` (one-shot), a barrier is **reusable** (cyclic), so it can gate every iteration of a loop - the bulk-synchronous pattern behind iterative solvers (Jacobi, conjugate gradient, time-stepping), where every thread must finish sweep *k* before any thread starts sweep *k+1*.

```c
import "barrier.cb";

barrier b;
b.init(workers);

// in each worker, every sweep:
compute_my_slice(src, dst);
bool leader = b.arrive_and_wait();   // wait for all workers to finish the sweep
if (leader) { swap(src, dst); }      // serial fixup runs on exactly one thread
b.arrive_and_wait();                 // re-sync so no worker reads a half-swapped state
// ...
b.destroy();
```

- `init(int n)` - set the number of participating threads. Call before any arrive.
- `arrive_and_wait()` - arrive and block until all `n` have arrived; the barrier then resets for the next phase. **Returns `true` for exactly one thread per phase** (the last arriver, the "leader"), so a single-threaded fixup - a buffer swap, a reduction combine - can run once between phases. (This is the equivalent of C++ `std::barrier`'s completion function or Rust `Barrier`'s `is_leader()`.)
- `arrive_and_drop()` - arrive *and* permanently leave the group, so later phases expect one fewer thread. Use when a worker is done for good.
- `destroy()` - release resources (no threads may be waiting).

**Two variants** (the same arrive/leader contract, mirroring `channel` vs `spsc_queue`):

| Type | Waiting | Use when |
|------|---------|----------|
| `barrier` | blocking - waiters sleep on a condition variable | the general default; does not burn CPU when work is imbalanced |
| `spin_barrier` | busy-wait - waiters spin on an atomic generation counter with `pause()` | fine-grained HPC sweeps where all workers arrive nearly simultaneously and are pinned to cores; keeps a core hot while waiting. Fixed-size (no `arrive_and_drop`). |

Both are correct across reuse via an internal **generation counter**: the last arriver advances the phase before releasing, so a fast thread that laps back into the next phase cannot let a straggler from the previous phase slip through.

### Thread Pool (`core/threadpool.cb`)

`ThreadPool` runs a fixed set of worker threads that pull tasks from an internal priority queue. Use it instead of raw `Thread` when you have many short tasks: the workers are created once and reused, so you avoid per-task thread create/teardown.

A task is a function `int(move void*, stop_token)`: it receives an owning context pointer and a cancellation token, and returns an exit code.

```c
import "threadpool.cb";

struct Work { int n = 0; };

int task(move void* raw, stop_token tok) {
    Work* w = (Work*)raw;
    int result = w->n * w->n;
    delete w;                 // the task owns ctx; free it when done
    return result;
}

ThreadPool pool;
pool.init(4);                 // 4 worker threads

Work* w = new Work; w->n = 7;
move TaskHandle* h = pool.submit(task, w);   // ownership of w moves into the pool
if (h != nullptr) {
    h->wait();                // block until the task finishes
    int code = h->exitCode;   // 49
}

pool.shutdown();              // drain, stop workers, release resources
```

**Ownership of `ctx`.** Once you pass `ctx` to `submit`/`submitDetached`/`then`, the pool owns it. On success the pointer is moved (still owning) into the task function, which must free it. If the pool *refuses* the task (returns `nullptr`/`false` - queue full or shutting down), the pool frees `ctx` itself. Either way the caller never frees `ctx` after handing it over.

**Submission methods:**

| Method | Returns | Use |
|--------|---------|-----|
| `submit(fn, ctx, priority?)` | `move TaskHandle*` (or `nullptr`) | wait on / cancel the task |
| `submitDetached(fn, ctx, priority?)` | `bool` | fire-and-forget; no handle |
| `then(dep, fn, ctx, priority?)` | `move TaskHandle*` (or `nullptr`) | run `fn` after `dep` finishes (call once per `dep`) |

`priority` is optional (defaults to `THREADPOOL_PRIORITY_NORMAL`); the other values are `THREADPOOL_PRIORITY_HIGH` and `THREADPOOL_PRIORITY_LOW`.

**`TaskHandle`** (returned owning by `submit`/`then`):

- `wait()` - block until the task completes (busy-spin yielding the core).
- `wait_for(int ms)` - block until done or timeout; returns `true` if it finished in time.
- `cancel()` - request cancellation; the task must check `tok.stop_requested()`.
- `exitCode` / `failed` - the task's return code and whether it was non-zero (valid after `wait`).
- Forgetting to `wait()` is safe: `~TaskHandle` blocks until the worker has finished before freeing.

**`TaskResult<T>`** - a typed result holder (the equivalent of a future) for returning a value rather than just an exit code. Bundle a `TaskResult<T>*` into your context; the task calls `set(v)` (or `fail()`), and the submitter calls `get()` to block and retrieve the value:

```c
struct SqCtx { int in = 0; TaskResult<int>* out = nullptr; };

int sq(move void* raw, stop_token tok) {
    SqCtx* c = (SqCtx*)raw;
    c->out->set(c->in * c->in);
    delete c;
    return 0;
}

TaskResult<int> r;
SqCtx* c = new SqCtx; c->in = 9; c->out = &r;
pool.submitDetached(sq, c);
int v = r.get();              // blocks; returns 81  (use r.hasFailed() if the task may fail)
```

**Lifecycle:**

- `drain()` - block until every submitted task has finished (workers stay alive).
- `shutdown()` - `drain()`, stop and join all workers, release resources; the pool can be `init()`ed again afterwards. The destructor calls `shutdown()` automatically.
- `resize(int newCount)` - grow or shrink the worker count at runtime. Not thread-safe against concurrent `submit`/`drain`/`shutdown` - quiesce the pool first.
- `workerCount()` / `approxPendingCount()` - introspection (the pending count is approximate, for diagnostics only).

> **Inside a `program`:** the pool runs fully multi-threaded inside `program` scope; worker threads cross-allocate safely against the program allocator. Size the internal queue via `THREADPOOL_QUEUE_CAPACITY` if bursts of `submit` outpace the workers (a saturated queue makes `submit` return `nullptr`/`false`).

---

## Compile-Time Lock-Set Analysis

CFlat performs **static lock-set analysis** - the compiler tracks which locks are held at each point and reports unguarded accesses or missing lock requirements as errors. No annotations are needed at call sites; the compiler derives requirements from declarations.

The `lock(...)` clause appears in four positions:

| Position | Purpose |
|----------|---------|
| Struct field group | Declare which fields require a lock to access |
| Lock statement body | Acquire one or more locks for the duration of a block |
| Function clause | Declare that callers must hold a lock (enforced at every call site) |
| Positional method group | Group methods that all require the same lock |

### Guarded Field Groups

Wrap fields in `lock(fieldName) { ... }` inside a struct to declare that the named mutex must be held before reading or writing those fields:

```c
import "mutex.cb";

struct Counter {
    mutex mtx = default;
    lock(mtx) {
        int value = default;   // requires mtx held to read or write
    }
};
```

Accessing `value` without holding `mtx` is a compile error:

```c
Counter c;
c.value = 1;       // error: must hold 'c.mtx' before accessing it
```

### Lock Statement

The `lock(expr)` statement acquires the lock before entering the block and releases it on exit. One or more comma-separated lock expressions are accepted:

```c
Counter c;

lock(c.mtx) {
    c.value = 42;      // c.mtx is in the lock-set here - OK
}

Counter c2;
lock(c.mtx, c2.mtx) {
    c.value = c.value + c2.value;   // both locks held - OK
}
```

### Function Clause (call-site contract)

Append `lock(expr)` after the parameter list to declare that the **caller** must hold a lock before calling the function. The compiler checks this at every call site:

```c
void increment(Counter* a, int n) lock(a.mtx) {
    a->value = a->value + n;   // a->mtx is in the lock-set inside the body
}
```

Calling `increment` without holding the required lock is a compile error:

```c
Counter c;
increment(&c, 1);             // error: must hold 'c.mtx' before calling this function

lock(c.mtx) {
    increment(&c, 1);         // OK: c.mtx is held
}
```

The `lock(expr)` clause is a **contract** - it is enforced at the call site, not just documented. The formal parameter name in the clause (`a`) is substituted with the actual argument name (`c`) at each call site, so the error message names the concrete variable:

```
error: must hold 'c.mtx' before calling this function
```

### Positional Method Groups

For structs where many methods share the same lock requirement, declare them inside the lock group. This avoids repeating a trailing `lock(this.mtx)` on every method:

```c
struct Account {
    mutex mtx = default;
    lock(mtx) {
        int balance = default;          // guarded field

        void deposit(int n) {           // implicitly requires mtx
            balance = balance + n;
        }

        int getBalance() {              // implicitly requires mtx
            return balance;
        }
    }
};
```

Methods inside the group use **standard CFlat method style** - no explicit self parameter; fields are accessed via implicit `this`. Calling these methods without holding the receiver's lock is a compile error:

```c
Account acct;

acct.deposit(100);            // error: must hold 'acct.mtx' before calling this function

lock(acct.mtx) {
    acct.deposit(100);        // OK
    int bal = acct.getBalance();
}
```

### Reader-Writer Locks

Use `lock(rw.read)` or `lock(rw.write)` to distinguish read and write acquisitions on an `rwlock`:

```c
import "rwlock.cb";

struct RwData {
    rwlock rw = default;
    lock(rw) {
        int data = default;
    }
};

RwData rd;
lock(rd.rw.read) {
    int v = rd.data;    // read lock held
}
lock(rd.rw.write) {
    rd.data = 42;       // write lock held
}
```

The `.read` / `.write` suffix is stripped when checking lock-set membership, so `lock(rw.read)` and `lock(rw.write)` both count as holding `rw` for the purposes of guarded-field checks.

### Atomic Memory-Ordering Guards

Mutexes provide mutual exclusion; atomics provide visibility ordering. CFlat extends the same lock-set analysis to lock-free code so the compiler can statically verify that every access to data protected by an atomic happens inside a correctly-ordered release or acquire scope.

#### The problem

Standard atomic operations carry a memory-ordering annotation (release, acquire, seq_cst), but nothing connects the **guard atomic** to the **data it protects**. A programmer can silently omit the ordering, use the wrong ordering, or access guarded fields entirely outside any fence:

```c
// nothing stops a bare write - no ordering, no happens-before edge
s.value = 42;
__atomic_i32_store(&s.ready, 1);  // wrong: seq_cst, not release; s.value may be reordered past
```

#### Declaring guarded fields

In a struct, use `lock(atomicField) { ... }` - the same syntax as a mutex guard group - to declare that the enclosed fields require an atomic release or acquire scope to access:

```c
import "atomic.cb";

struct SharedData {
    atomic__i32 ready = default;
    lock(ready) {
        int value = 0;      // requires release/acquire scope to read or write
    }
};
```

Accessing `value` without a release or acquire scope is a compile error:

```c
SharedData s;
s.value = 42;           // error: Field 'value' is guarded by 'ready'
int x = s.value;        // error: Field 'value' is guarded by 'ready'
```

#### Producer - `release`

Call `release` on the guard atomic to publish guarded fields. The lambda body executes first (data writes), then the atomic store is emitted with `memory_order_release`, establishing a happens-before edge:

```c
SharedData s;
s.ready.release(1, () => {
    s.value = 42;       // OK: inside release scope; compiler confirms 'ready' guards 'value'
});
// equivalent to: s.value = 42; atomic_store(&s.ready, 1, release)
```

#### Consumer - `acquire`

Call `acquire` on the guard atomic to observe guarded fields. The atomic load is emitted with `memory_order_acquire` first, then the lambda body executes (data reads):

```c
s.ready.acquire((i32 v) => {
    int x = s.value;    // OK: inside acquire scope; happens-after the producer's release
});
// equivalent to: v = atomic_load(&s.ready, acquire); use s.value ...
```

The loaded value is passed to the lambda as its argument. The consumer typically spins until the value signals ready:

```c
while (s.ready.load() == 0) {}    // spin-wait (plain seq_cst load)
s.ready.acquire((i32 v) => {
    process(s.value);
});
```

#### `lock(this)` on function parameters

The `release` and `acquire` methods work because their `Lambda<>` parameter is declared with `lock(this)`. (The body captures the guarded fields, so it is a fat `Lambda<>` closure, not a thin `function<>` - see [Function Pointers](LANGUAGE.md#function-pointers).)

```c
// simplified signature in atomic.cb
void release(i32 val, lock(this) Lambda<void()> body);
void acquire(lock(this) Lambda<void(i32)> body);
```

`lock(this)` is a parameter qualifier that tells the compiler: *when analysing the lambda passed here, seed `currentLockSet` with the call-site receiver.* At `s.ready.release(...)`, the receiver is `s.ready`, so the lambda body is checked as though `lock(s.ready)` was in effect - which satisfies the `GuardedBy` check on `s.value`.

You can use the same qualifier in your own functions to propagate a guard into a callback:

```c
void withData(SharedData* d, lock(this) Lambda<void()> body) lock(d->ready) {
    body();
}
```

> `lock(this)` is only accepted on a callback parameter (a `function<>` or `Lambda<>`). Using it on any other parameter type is a compile error.

#### Available atomic guard types

| Type | `release` / `acquire` | Notes |
|------|-----------------------|-------|
| `atomic__i32` | `release(i32, fn)` / `acquire(fn<void(i32)>)` | General-purpose guard counter or flag |
| `atomic__i64` | `release(i64, fn)` / `acquire(fn<void(i64)>)` | 64-bit version counter |
| `atomic_flag` | `release(bool, fn)` / `acquire(fn<void(bool)>)` | One-shot boolean flag (no `load()`) |

Use `atomic__i32` when you need to poll the guard value in a spin loop before calling `acquire`; `atomic_flag` has no `load()` method and cannot be polled.
