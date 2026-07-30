# CompileCoreOnly can cache a bodyless rebox thunk into core bitcode

Filed alongside the deferred-interface-rebox fix. Latent: confirmed by reading and
by inspecting the live cache, but unreachable today.

## Root cause

`CompileCoreOnly` (`cflat/LLVMBackend.cpp` ~4127, the `--init` path) compiles only
`runtime.cb` and never calls `EmitDeferredInterfaceReboxBodies` - deliberately, the
same way it never calls `EmitDeferredFullDestructorBodies`. So any
`__iface_rebox.<dstIface>` thunk handed out while compiling core would be written
into `core_<platform>.bc` with **internal linkage and no body**.

The load side is already handled: `AdoptInterfaceReboxThunksFromModule`
(`cflat/LLVMBackend.h` ~10158) re-adopts bodyless thunks out of a cached module so
finalization fills them in. Nothing guards the WRITE side.

## Why it is unreachable today

No core file reachable from `runtime.cb` declares an interface, so no core compile
produces an interface-to-interface conversion. Verified against the live cache
`~/.cflat/runtime/<hash>/core_macos.bc`: zero `__iface_rebox` symbols.

## Failure mode when it becomes reachable

Add an interface-to-interface conversion to `runtime.cb` (or anything it imports)
and `SaveCoreBitcode` writes a module with a bodyless internal function. A verifier
run rejects it: "Global is external, but doesn't have external or weak linkage".
Warm-cache compiles would then fail for every program until `~/.cflat` is cleared.

## Fix direction

Cheapest correct guard: in `CompileCoreOnly`, before `SaveCoreBitcode`, assert the
deferred rebox vector is empty and `LogError` if it is not, so the failure lands as
a compiler diagnostic at `--init` time instead of a verifier abort on every later
compile. Alternatively drain the vector there the way `Compile` does - but that
needs care, since core is compiled without the user's classes and the zero-case
diagnostic must stay suppressed for import-only sites.
