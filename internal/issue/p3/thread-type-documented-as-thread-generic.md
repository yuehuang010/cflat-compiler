# Docs name a `thread<T>` that does not exist; the real type is `Thread`

`doc/LANGUAGE.md` (core library table), `internal/stdlib-reference.md` (Concurrency table)
and `doc/CLI.md` (the `--run` restriction paragraph) all describe `core/thread.cb` as
exporting "`thread<T>` - Win32 thread wrapper". `core/thread.cb` declares no such type.

## What is actually there

```
cflat/core/thread.cb:193:  struct Thread {
    bool start(function<int(void*)> fn, void* ctx, int fpConfig = 0);       // borrow ctx
    bool start(function<int(move void*)> fn, move void* ctx, int fpConfig = 0);
    i32  join();
    bool try_join(i32 timeout_ms);
```

Not generic, not lowercase, and the payload crosses as a `void*` context pointer rather
than a type parameter. The description "Win32 thread wrapper" is also stale - the file has
`if const (__MACOS__)` arms and runs fine on macOS arm64.

## Impact

`thread<int> t;` is the first thing a reader of the docs writes and it does not resolve.
The correct shape (a context struct + `(void*)&ctx` + a `function<int(void*)>` entry) is
not derivable from any document; it has to be read out of `core/thread.cb`.
Cost me the whole first attempt at `scratch/dogfood/lang/wordfreq.cb`.

## Fix direction

Docs-only. Replace the three `thread<T>` mentions with `Thread`, drop "Win32", and add a
five-line worked example (context struct, `start(worker, (void*)&ctx)`, `join()` returning
the worker's `int`) to the Concurrency section of `doc/LANGUAGE.md`. Note in the same place
that the borrow overload requires the caller to keep `ctx` alive until `join()` returns.
