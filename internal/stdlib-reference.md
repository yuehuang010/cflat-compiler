---
name: stdlib-reference
description: CFlat standard library (core/) full table - all core/*.cb files, what they export, and import requirements
metadata:
  type: reference
---

# Standard Library Reference

The `core/` directory is implicitly added to the import search path by the compiler. Only `runtime.cb` is automatically imported; all other core libraries must be explicitly imported with `import "filename.cb"`.

Method-level API discovery is via the LSP (hover / completion / `--symbol`); this table is a file index of what each library is for. Grouped by category.

### Types & collections

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>`, `IReflector`, `ITuple<T...>` |
| `string.cb` | `string` - owned/borrowed UTF-8 value type; `IString` implementation |
| `wstring.cb` | `wstring` - owned UTF-16 string for Win32 `...W` / WinRT APIs |
| `array.cb` | `array<T>` - owning fixed-size heap array with destructor support |
| `list.cb` | `list<T>` - growable array |
| `span.cb` | `span<T>` - non-owning NOALIAS window (`T[]` + len); `as_view()` decays to `view<T>`. See `doc/HPC.md` |
| `view.cb` | `view<T>` - non-owning MAY-ALIAS window (`T*` + len); `slice(start,end)`; sibling of `span<T>` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map |
| `stack.cb` | `stack<T>` - LIFO |
| `queue.cb` | `queue<T>` - FIFO |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `tuple.cb` | `tuple<T...>` - variadic heterogeneous value type |
| `function.cb` | Closure-environment memory primitives backing `function<T>` / lambdas |
| `guid.cb` | `Guid` - 128-bit GUID / RFC 4122 UUID |

### Math & numerics

| File | Exports |
|------|---------|
| `math.cb` | `Math` namespace: `abs`/`min`/`max`/`pow`/`sqrt`/`clamp`/`fma`, trig, rounding; constants `Math.PI` / `TAU` / `E` / `SQRT2` / `LN2` / `LN10` |
| `random.cb` | `Random` - splitmix64 PRNG; independent substreams via `jump`/`split` |
| `intrinsic.cb` | Compiler intrinsics: `popcount`, `ctz`, `clz`, `prefetch`, `likely`/`unlikely`, `pause()` (x86 spin-loop hint) |

### Memory & allocators

| File | Exports |
|------|---------|
| `memory.cb` | `operator new`/`delete` and the active-allocator slot |
| `arena.cb` | `Arena` - bump allocator |
| `arena_allocator.cb` | Mixed-size bump arena backed by `g_page_pool` |
| `page_pool.cb` | Process-global pool of 64 KB pages for slab/arena backing |
| `page_arena.cb` | Self-contained bump-allocation region for zero-copy transfer |
| `vmem.cb` | OS virtual-memory abstraction |
| `block_allocator.cb` | Block-based pooled allocator (used by `program` thread startup) |
| `bucket_allocator.cb` | Segregated free-list allocator (size-class buckets) |
| `entry_allocator.cb` | Variable-size bump allocator with per-page entry tables |
| `fit_allocator.cb` | Fit allocation strategy |
| `malloc_allocator.cb` | Thin wrapper around system malloc/free |

### Concurrency

| File | Exports |
|------|---------|
| `thread.cb` | `thread<T>` - Win32 thread wrapper |
| `threadpool.cb` | `ThreadPool` - priority work queue with `submit`/`then`/`drain`; `TaskHandle`, `TaskResult<T>` |
| `mutex.cb` | `mutex`, `atomic<T>`, `lock` statement - synchronization primitives |
| `atomic.cb` | `Atomic<T>` - load/store/fetchAdd over LLVM atomic IR |
| `semaphore.cb` | `semaphore` - counting semaphore |
| `latch.cb` | `latch` - one-shot countdown latch |
| `rwlock.cb` | `rwlock` - reader-writer lock |
| `barrier.cb` | Reusable (cyclic) thread barriers |
| `channel.cb` | `channel<T>` - blocking MPMC queue |
| `arena_channel.cb` | Bucket-backed allocator + zero-copy MPMC channel |
| `spsc_queue.cb` | `spsc_queue<T>` - wait-free single-producer/single-consumer ring |
| `stop_token.cb` | `stop_token` / `stop_source` - cooperative cancellation |
| `program.cb` | `program` construct runtime support (thread + allocator lifecycle) |

### I/O, system & time

| File | Exports |
|------|---------|
| `filesystem.cb` | `File`, `Path`, `Directory` - file and directory operations |
| `cruntime.cb` | Raw C runtime: `printf` family, stdout/stdin writers (`puts`/`putchar`/`putn`/`getchar`), `string.h`/`ctype.h`. Hook-aware (work inside a `program`) and VT-correct; prefer `putn(buf, len)` over Win32 `WriteFile` for bulk output |
| `terminal.cb` | Win32 console terminal utilities |
| `stream.cb` | Double-buffered text pipe between two `program` constructs |
| `process.cb` | Launch and communicate with a child process |
| `time.cb` | `TimePoint`, `Stopwatch`, `Timer`, `sleep`, `rdtscp()`/`lfence()` (x86 timing) |
| `os.cb` | Platform dispatcher (imports `os.windows.cb` on Windows) |
| `os.windows.cb` | Windows platform externs: Win32 API and MSVC CRT |
| `network/socket.cb` | Platform-agnostic TCP/UDP socket API |
| `network/socket.windows.cb` | Winsock2 raw bindings (pulled in by `socket.cb` on Windows) |

### Serialization & text

| File | Exports |
|------|---------|
| `json.cb` | `JsonBuilder.toJson<T>` / `fromJson<T>` - JSON serialization |
| `xml.cb` | `XmlBuilder` - struct-to-XML via the `reflect` intrinsic |
| `regex.cb` | `Regex` - Thompson-NFA (linear-time) regex; capture groups, case-insensitive. See `doc/REGEX.md` |

### COM / WinRT

| File | Exports |
|------|---------|
| `com.cb` | Shared COM/WinRT helpers: well-known IIDs and HRESULT codes. See `doc/WINMD.md` |
| `winrt.cb` | WinRT runtime plumbing: HSTRING bridge, activation, async polling. See `doc/WINMD.md` |

### Graphics

| File | Exports |
|------|---------|
| `graphic/bitmap.cb` | `Bitmap` - load/create/save Windows BMP files; `BitmapPixel` struct |

### HPC (see `doc/HPC.md`)

| File | Exports |
|------|---------|
| `hpc/parallel.cb` | `parallel_for_n` / `parallel_reduce<T>` (raw-thread) and `..._pool` (ThreadPool) - across-core data parallelism over `[0,n)` |
| `hpc/vecmath.cb` | BLAS level-1 style dense vector kernels over `double[]` |
| `hpc/densemat.cb` | Dense row-major matrix kernels (BLAS level-2/3 style) |
| `hpc/factor.cb` | Dense matrix factorization + triangular solve |
| `hpc/sparse.cb` | Compressed sparse row (CSR) matrix and sparse mat-vec |
| `hpc/solvers.cb` | Iterative sparse linear solvers |
| `hpc/stencil.cb` | Structured-grid stencil kernels (finite-difference PDEs) |
| `hpc/scan.cb` | Reductions, prefix sums, and streaming statistics over `double[]` |
| `hpc/fft.cb` | Iterative in-place radix-2 Cooley-Tukey FFT |

### Diagnostics

| File | Exports |
|------|---------|
| `diagnostic/heap_audit.cb` | Opt-in heap leak detector (front end). See `doc/CLI.md` `--heap-audit` |
| `diagnostic/thread_fuzz.cb` | Seeded randomized thread-scheduling fuzzer |

To add a new core library: add the `.cb` file to `core/` and add an entry in `cflat.vcxproj` with `DeploymentContent`.
