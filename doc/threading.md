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
- [Compile-Time Lock-Set Analysis](#compile-time-lock-set-analysis)
  - [Guarded Field Groups](#guarded-field-groups)
  - [Lock Statement](#lock-statement)
  - [Function Clause (call-site contract)](#function-clause-call-site-contract)
  - [Positional Method Groups](#positional-method-groups)
  - [Reader-Writer Locks](#reader-writer-locks)
  - [Atomic Memory-Ordering Guards](#atomic-memory-ordering-guards)

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

**`delete[n]`** — call destructors on exactly `n` elements, then free. Use when the array was allocated without a cookie (e.g. raw `malloc`/`operator new`) and you know the count:

```c
Node* nodes = new Node[5];
delete[5] nodes;    // calls ~Node() on each of the 5 elements, then frees
```

**`delete[_]`** — free the backing buffer *without* calling any destructors. Use after you have already destroyed elements manually (e.g. with `.~()`):

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
| `delete[_] p` | none | — |


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

Use the `lock` statement for scoped acquisition — the lock is released automatically at the end of the block:

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

`atomic.cb` also provides low-level value-typed atomics — `atomic__i32`, `atomic__i64`, and `atomic_flag` — used when you need release/acquire ordering or want to guard associated fields (see [Atomic Memory-Ordering Guards](#atomic-memory-ordering-guards)):

```c
import "atomic.cb";

atomic__i32 seq = default;
seq.store(1);
int v = seq.load();
seq.fetchAdd(1);
```

> `atomic_flag` is a spinlock primitive (`test_and_set` / `clear`); it has no `load()`. Use `atomic__i32` when you need to poll a flag value.

### Other Synchronization Primitives

- `core/semaphore.cb` — `Semaphore` with `acquire`/`release`
- `core/latch.cb` — `Latch` countdown; `countDown`, `wait`
- `core/rwlock.cb` — `RwLock`; `acquireRead`/`releaseRead`, `acquireWrite`/`releaseWrite`
- `core/channel.cb` — `channel<T>` blocking MPMC queue; `send`, `recv`, `tryRecv`
- `core/spsc_queue.cb` — `spsc_queue<T>` wait-free single-producer/single-consumer ring

```c
import "channel.cb";

channel<int> ch;
ch.init(1024);
ch.send(42);
int v = ch.receive();   // 42
```

---

## Compile-Time Lock-Set Analysis

CFlat performs **static lock-set analysis** — the compiler tracks which locks are held at each point and reports unguarded accesses or missing lock requirements as errors. No annotations are needed at call sites; the compiler derives requirements from declarations.

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
    c.value = 42;      // c.mtx is in the lock-set here — OK
}

Counter c2;
lock(c.mtx, c2.mtx) {
    c.value = c.value + c2.value;   // both locks held — OK
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

The `lock(expr)` clause is a **contract** — it is enforced at the call site, not just documented. The formal parameter name in the clause (`a`) is substituted with the actual argument name (`c`) at each call site, so the error message names the concrete variable:

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

Methods inside the group use **standard CFlat method style** — no explicit self parameter; fields are accessed via implicit `this`. Calling these methods without holding the receiver's lock is a compile error:

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
// nothing stops a bare write — no ordering, no happens-before edge
s.value = 42;
__atomic_i32_store(&s.ready, 1);  // wrong: seq_cst, not release; s.value may be reordered past
```

#### Declaring guarded fields

In a struct, use `lock(atomicField) { ... }` — the same syntax as a mutex guard group — to declare that the enclosed fields require an atomic release or acquire scope to access:

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

#### Producer — `release`

Call `release` on the guard atomic to publish guarded fields. The lambda body executes first (data writes), then the atomic store is emitted with `memory_order_release`, establishing a happens-before edge:

```c
SharedData s;
s.ready.release(1, () => {
    s.value = 42;       // OK: inside release scope; compiler confirms 'ready' guards 'value'
});
// equivalent to: s.value = 42; atomic_store(&s.ready, 1, release)
```

#### Consumer — `acquire`

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

The `release` and `acquire` methods work because their `function<>` parameter is declared with `lock(this)`:

```c
// simplified signature in atomic.cb
void release(i32 val, lock(this) function<void()> body);
void acquire(lock(this) function<void(i32)> body);
```

`lock(this)` is a parameter qualifier that tells the compiler: *when analysing the lambda passed here, seed `currentLockSet` with the call-site receiver.* At `s.ready.release(...)`, the receiver is `s.ready`, so the lambda body is checked as though `lock(s.ready)` was in effect — which satisfies the `GuardedBy` check on `s.value`.

You can use the same qualifier in your own functions to propagate a guard into a callback:

```c
void withData(SharedData* d, lock(this) function<void()> body) lock(d->ready) {
    body();
}
```

> `lock(this)` is only accepted on `function<>` parameters. Using it on any other parameter type is a compile error.

#### Available atomic guard types

| Type | `release` / `acquire` | Notes |
|------|-----------------------|-------|
| `atomic__i32` | `release(i32, fn)` / `acquire(fn<void(i32)>)` | General-purpose guard counter or flag |
| `atomic__i64` | `release(i64, fn)` / `acquire(fn<void(i64)>)` | 64-bit version counter |
| `atomic_flag` | `release(bool, fn)` / `acquire(fn<void(bool)>)` | One-shot boolean flag (no `load()`) |

Use `atomic__i32` when you need to poll the guard value in a spin loop before calling `acquire`; `atomic_flag` has no `load()` method and cannot be polled.
