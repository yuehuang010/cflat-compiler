# `new D(x)` on a [cpp] struct deriving from a C++ base skips D's own operator new

Found by T313 review 3 (2026-10-16), PRE-EXISTING on master. `[cpp] struct D : ri.PD` (PD a C++ base declaring
operator new/delete) with its OWN `operator new`: `new D(x)` allocates without calling `D.operator new` but frees through
`PD::operator delete` - an allocator/deallocator mismatch. The tag form `<D .../>` calls D.operator new correctly.
Repro: scratch/repro_keep/box1016b/t313/rev_t313_r3_inhDargs.cb (header rev_t313_rv.hpp beside it, compile with -i that dir).

## Fix direction

The `new T(args)` path (cflat/MainListener_Expressions.cpp ~25384) resolves T.operator new by mangled name only on T;
the [cpp]-struct ctor path for a derived class likely takes a different branch. Make it look up D's own operator new
first (clang: class scope, then bases, then global), and pair the release with LLVMBackend::EmitUserNewHeapFree.
