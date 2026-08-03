# A `const`-qualified C function-pointer parameter binds as `void*`, and the code-value gate then rejects the call

Filed 2026-08-03 while verifying the `void*` half of
`funcptr-refuted-candidate-rebinds-onto-pointer-sibling` as an oracle before widening it. **The
first filing of this file named the wrong qualifier and the wrong repro; both are corrected here
from re-measurement.** See "What this is NOT" below - the correction is the useful part.

Severity: **P2 - false rejection of a legal C-interop call.** A correct program is hard-errored.

## Repro

`scratch/fpr_r_cb.h`:

```c
int regPlain(int (*cb)(int));
int regConst(int (* const cb)(int));
```

```cflat
import package "fpr_r_cb.h";
int cb(int x) { return x; }
extern int main() { regConst(cb); return 0; }
```

```
fpr_r_callconst.cb(3,20): no overload of 'regConst' matches the given arguments.
  Candidates (1):
    regConst(void* cb)
```

Exit 1, identical on `904f026` and on `fix/funcptr-rebind` - the gate that rejects it is the
pre-existing `void*` one, not the widening. `regPlain(cb)` in the same header compiles.

`--symbol` shows the two parameters typed differently from the same header:

```
regPlain                 int regPlain(__c_fn_ptr cb)
regConst                 int regConst(void* cb)
```

## Root cause

`MapCTypeToTypeAndValueImpl` (`cflat/LLVMBackend.h`) detects a C function-pointer spelling by
searching for the literal substring `"(*)"`. clang spells this parameter `int (* const)(int)`, so
the marker is `"(* const)"` and the check misses; the spelling then falls through the generic path,
which counts one `*` and yields `void*`. `AggregatePointeeTag` a few lines above already strips
`const` / `volatile` / `restrict` / `_Nonnull` / `_Nullable` / `_Null_unspecified` before counting
stars - the function-pointer marker search does not.

The rejection itself is correct given the recorded parameter type: a function VALUE does not
convert to a `void*` data parameter. The defect is upstream, in what the parameter was recorded as.

## What this is NOT

- **Nullability decoration does NOT trigger it.** Measured: `void (* _Nonnull cb)(void)`,
  `void (* _Nonnull)(void)`, and `int (* _Nonnull cb)(int)` all bind as `__c_fn_ptr` and call
  cleanly. clang's canonical spelling keeps `(*)` intact for those. The first filing of this file
  claimed `_Nonnull` was the trigger and was wrong.
- **`atexit(bye)` is NOT this bug**, and was the wrong repro to file it under. The
  `atexit(void* func)` candidate that rejects that call is `cflat/core/cruntime.cb:584`, a
  HAND-WRITTEN `extern int atexit(void* func);`. Proof: `--symbol atexit` on a file with no C
  import at all reports it, `defined: .../core/cruntime.cb:584`. The binder parses the SDK's
  `atexit` correctly - with `import package "stdlib.h"` alone `--symbol atexit` lists BOTH
  `int atexit(__c_fn_ptr)` and `int atexit(void* func)`. Two further claims in the first filing
  were also false and are withdrawn: that stdlib.h alone yields only the `void*` form, and that
  importing `stdlib.h` + `signal.h` + `pthread.h` makes the call bind (it does not - the call still
  reports `Candidates (1): atexit(void* func)`, so only the cruntime prototype reaches overload
  resolution even when both are indexed).

## Two separable fixes

1. **This file**: strip the cv / nullability qualifier words before the `"(*)"` marker search, the
   same list `AggregatePointeeTag` and the generic path already use, then re-probe. Pointee
   spellings inside the signature are resolved recursively and already strip them, so only the
   marker search is affected. Prove the accept set first: the probe currently rejects anything it
   cannot parse rather than silently accepting it, so a looser marker must not start claiming
   declarations that are not function pointers.

2. **`cflat/core/cruntime.cb:584`**, separately: `extern int atexit(void* func);` should be
   `extern int atexit(function<void()> func);` so `atexit(bye)` binds. NOT done with the funcptr
   work, and deliberately not folded in here: retyping a core prototype narrows what it accepts
   (`atexit(someVoidPtr)` is legal today) and needs its own accept-set sweep. Note also that
   whichever prototype is fixed, the SDK overload does not currently participate in resolution, so
   fixing only the header side would not close it.
