Bucket: p3 (--run only; AOT correct; clean exit otherwise)

# `--run` exits 139 at JIT teardown when a header-defined C++ static has a destructor

Found 2026-09-17 in review of fix/cpp-global-ctor (macOS arm64, Release). Pre-existing: reproduces
with NO CFlat global present, on master 884a0c08 (before that fix) and after it.

## Summary

A header-defined `inline` namespace-scope C++ object with a user destructor (clang registers the
destructor via `__cxa_atexit` inside its `__cxx_global_var_init`) constructs and prints correctly
under `--run`, `main` returns 0, and the process then dies with signal 11 during JIT teardown.
The AOT executable built from the same source runs the destructor and exits 0.

## Repro

`scratch/rev2_dtor.h`
```cpp
#pragma once
#include <cstdio>
namespace rev2 {
struct Sink { int magic; Sink(); ~Sink(); };
inline Sink::Sink() : magic(4242) { printf("[Sink ctor]\n"); }
inline Sink::~Sink() { printf("[Sink dtor]\n"); }
inline Sink g_sink;
}
```

`scratch/jitdtor_probe.cb`
```cflat
import cpp "rev2_dtor.h";
extern int main() { printf("main mag=%d\n", rev2.g_sink.magic); return 0; }
```

Measured (master 884a0c08 binary):
```
cflat scratch/jitdtor_probe.cb -i scratch --run   -> [Sink ctor] / main mag=4242 / exit 139 (no dtor line)
cflat scratch/jitdtor_probe.cb -i scratch -o a.out; ./a.out -> [Sink ctor] / main mag=4242 / [Sink dtor] / exit 0
```

## Root cause (hypothesis, not measured)

The `__cxa_atexit` callback registered by the companion module points into JIT-owned memory;
the JIT (or the whole process image) is torn down before the C runtime's exit handlers run, so
the handler jumps into unmapped code. Either the JIT path must run `jit->deinitialize()` (which
runs the registered atexit handlers via the ORC platform) before destroying the JIT, or the
program's `exit` must be sequenced so the handlers run while the code is still mapped.

## Fix direction

In the `--run` path (LLVMBackend_EmitAndLink.cpp, around the `jit->initialize` call ~4086), pair
initialize with `deinitialize` after `main` returns and before the JIT is destroyed; verify with
the repro that `[Sink dtor]` prints and the exit code is 0 under `--run`. Add a leg in an existing
--run-capable fixture only if one already exercises a C++ header static.
