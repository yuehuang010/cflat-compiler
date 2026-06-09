# core/parallel.cb authoring gotchas (parallel_for_n / parallel_reduce)

core/parallel.cb LANDED 2026-06-07 (gap #3 of [[project_multithread_hpc_eval]]). Across-core data parallelism over `[0,n)`: balanced chunk math + spawn/join (or submit/wait) + partials/combine, packaged so kernels stop hand-rolling it.

**API (4 free functions):**
- `parallel_for_n(n, workers, function<void(i64 lo, i64 hi, void*)> body, void* ctx)` - raw thread.
- `parallel_for_n_pool(ThreadPool* pool, n, workers, body, ctx)` - pool.
- `parallel_reduce<T>(n, workers, T identity, function<T(i64,i64,void*)> partial, function<T(T,T)> combine, void* ctx)` - raw thread.
- `parallel_reduce_pool<T>(pool, n, workers, identity, partial, combine, ctx)` - pool.
- `workers<=0` -> hardware_concurrency() (or pool->workerCount()); `workers` clamped to n. body/partial read buffers via borrowed `void* ctx` (lambdas don't capture). combine must be associative; result depends on worker count (reassoc), not bit-identical to serial.

**Design:** raw uses Thread.start(borrow fn, &box) over one `new __PForBox[workers]` array, join, delete[_]. Pool uses `new __PForBox`/`__PRedBox<T>` per task (move into submit; trampoline deletes), collects TaskHandle* and waits. partials = `T* partials = new T[workers]` (T[] decays to T*), each worker writes its own slot `&partials[w]` (no sync), helper combines after join. Test/test_parallel.cb 6/6 (axpy map + dot/i64-sum reductions, both backends, workers>n edge). Full test.bat green.

**FOUR CFlat gotchas burned (all worked around, none are compiler fixes):**
1. **`new T*[n]` (heap array of bare pointers) does NOT parse** ("extraneous input '['"). Workaround: wrap in a 1-field struct `struct __HandleSlot { TaskHandle* h; }` and `new __HandleSlot[n]`.
2. **Lambda param lists reject `move`** ("no viable alternative at input '(move void*'"). So a `function<int(move void*, stop_token)>` (the ThreadPool task ABI) CANNOT be an inline lambda - must be a top-level function.
3. **A generic function cannot be referenced as a function-pointer VALUE** - `pool->submit(__pred_pool_tramp<T>, box)` fails ("no overload of submit matches", the fn arg vanishes). Combined with #2: the pool reduce trampoline can't be generic-and-move. SOLUTION = store a non-capturing generic lambda `function<int(void*)> run` (lambdas CAN return values + this type is T-independent) as the FIRST field of `__PRedBox<T>`, and dispatch through a NON-generic top-level `__pred_pool_tramp(move void*, stop_token)` that casts raw to a layout-twin header `struct __PRedHeader { function<int(void*)> run; }` and calls `hdr->run(raw)`. The header-alias trick gives type erasure without the compiler's generic-fnptr or move-lambda limits.
4. **Two same-named GENERIC overloads collide** - `parallel_reduce<T>(...6 args)` and `parallel_reduce<T>(pool,...7 args)` both monomorphize to symbol `parallel_reduce__double` (mangling ignores value params) -> one instantiates with empty signature, resolution fails. Non-generic overloads (parallel_for_n's two forms) are FINE. Fix: `_pool` suffix on both pool variants for a uniform API (don't overload generics by arity).

OTHER: **a value-returning lambda passed INLINE as a call arg emits with void return type** ("Found return instr that returns non-void in Function of void return type", IR verify fail). A value-returning lambda assigned to a typed LOCAL first works (the `run` field proves it). So reduce partial/combine must be named functions or typed-local lambdas, NOT inline args. void-returning body lambdas (parallel_for_n) inline fine. `delete <struct field>` is rejected - extract via `T* p = move s.field; delete p;`.

GOTCHA recall: editing cflat/core/parallel.cb needs a Release rebuild to redeploy to x64/Release/core (same stale-deploy trap as [[project_span_view_types]]).
