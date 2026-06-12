---
name: stdlib-reference
description: CFlat standard library (core/) full table - all core/*.cb files, what they export, and import requirements
metadata:
  type: reference
---

# Standard Library Reference

The `core/` directory is implicitly added to the import search path by the compiler. Only `runtime.cb` is automatically imported; all other core libraries must be explicitly imported with `import "filename.cb"`.

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>` - requires `import "interfaces.cb"` |
| `string.cb` | `string` value type, manipulation, `IString` implementation - requires `import "string.cb"` |
| `array.cb` | `array<T>` - fixed-size heap array; `init(n)`, `get(i)`, `set(i, v)`, `operator[]`; iterable with `for` |
| `span.cb` | `span<T>` - non-owning NOALIAS window (`T[] _ptr` + len); pass by value, `data()` to a local `T[]` for vectorization; `as_view()` decays to `view<T>`. See `doc/HPC.md` |
| `view.cb` | `view<T>` - non-owning MAY-ALIAS window (`T* _ptr` + len); `slice(start,end)` for sub-ranges; the permissive sibling of `span<T>` |
| `list.cb` | `list<T>` - growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, `fma` (fused multiply-add), trig, rounding; constants `Math.PI` / `TAU` / `E` / `SQRT2` / `LN2` / `LN10` |
| `stack.cb` | `stack<T>` - LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` - FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |
| `thread.cb` | `thread<T>` - Win32 thread wrapper; requires `import "thread.cb"` |
| `random.cb` | Splitmix64 PRNG; requires `import "random.cb"` |
| `channel.cb` | `channel<T>` - MPMC blocking channel; requires `import "channel.cb"` |
| `spsc_queue.cb` | `spsc_queue<T>` - wait-free single-producer/single-consumer ring buffer |
| `mutex.cb` | `mutex`, `atomic<T>`, `lock` statement - thread synchronization primitives |
| `latch.cb` | `latch` - one-shot countdown synchronization |
| `semaphore.cb` | `semaphore` - counting semaphore |
| `rwlock.cb` | `rwlock` - reader-writer lock |
| `stop_token.cb` | `stop_token` / `stop_source` - cooperative cancellation |
| `threadpool.cb` | `ThreadPool` - priority work queue with `submit`/`then`/`drain`; `TaskHandle`, `TaskResult<T>` |
| `hpc/parallel.cb` | `parallel_for_n` / `parallel_reduce<T>` (raw-thread) and `parallel_for_n_pool` / `parallel_reduce_pool<T>` (ThreadPool) - across-core data parallelism over `[0,n)`; `import "hpc/parallel.cb"`. See `doc/HPC.md` |
| `time.cb` | `TimePoint.now()`, `Stopwatch`, `Timer`, `sleep`, `rdtscp()` (x86 cycle counter for jitter timing), `lfence()` (x86 serializing load fence), duration utilities |
| `intrinsic.cb` | Compiler intrinsics: `popcount`, `ctz`, `clz`, `prefetch`, `likely`/`unlikely` (target-independent), `pause()` (x86 spin-loop hint, used by `spsc_queue`/`channel` spin loops) |
| `tuple.cb` | `tuple<A,B,C>` - fixed-arity generic struct |
| `json.cb` | `JsonBuilder.toJson<T>` / `fromJson<T>` - JSON serialization |
| `regex.cb` | `Regex.compile` / `isMatch` / `find` / `findAll` / `replace` / `split`, capture groups, case-insensitive - NFA (Thompson, linear-time) regex; requires `import "regex.cb"`. See `doc/REGEX.md` |
| `arena.cb` | Arena bump allocator |
| `block_allocator.cb` | Block-based pooled allocator (used by `program` thread startup) |
| `malloc_allocator.cb` | Thin wrapper around system malloc/free |
| `fit_allocator.cb` | Fit allocation strategy |
| `bucket_allocator.cb` | Segregated free-list allocator (size-class buckets) |
| `program.cb` | `program` construct runtime support (thread + allocator lifecycle) |
| `cruntime.cb` | Raw C runtime bindings: formatted I/O (`printf`/`fprintf`/`sprintf`/`snprintf`/`scanf`/`sscanf`/`fscanf`), stdout convenience writers (`puts`, `putchar`, `putn` for raw bulk bytes, `fputs`, `fputc`/`putc`), stdin readers (`getchar`, `getc`), and `string.h`/`ctype.h` (memcpy, strlen, etc.). Stdout/stdin functions are hook-aware (work inside a `program`) and VT-correct. Use `putn(buf, len)` for bulk terminal output instead of Win32 `WriteFile`. |

To add a new core library: add the `.cb` file to `core/` and add an entry in `cflat.vcxproj` with `DeploymentContent`.
