# test_cpp_interop.cb still fails at -O2 (non-unwind leftovers)

Summary: the unwind root cause is fixed (the generated `[cpp] struct` constructor helpers
`__cflat_ctor_<S>_<n>` were declared `noexcept` in the synthesized C++, so clang marked the C++
constructor and its call nounwind and a field constructor's throw terminated once the optimizer
inlined the frames; legs 3276-3277, twin-run at -O2 in Test/test_operators.cb). Scanning the whole
file at -O2 (each failing leg neutralized in a scratch copy, then recompiled) leaves two unrelated
causes. Until both are fixed the whole file cannot take `// cflat-twin-args: -O2`.

## 1. Leg 3286: a CFlat-defined C library name is folded as the libc builtin

Repro: `extern i64 atoll(const char* s) { ...throws when armed... }` plus core `string.toLong()` on
the literal "42" (`UnwCoreOverridden`). At -O1/-O2 LLVM's LibCallSimplifier folds `atoll("42")` to
42 after toLong is inlined, so the throw never happens (guarded returns 42, not -5). The call to the
CFlat body carries no `nobuiltin`. Oracle: clang++ -O2 gives the same fold when the user `atoll` is
`noinline` (prints `42 42`); clang only passes by inlining the body first.
Fix direction: a CFlat body for a name TargetLibraryInfo knows (core binds to it on purpose) should
get `nobuiltin` on the function so every call site stops being a libcall; needs a ruling on whether
overriding a C library name is a supported CFlat feature at -O2 (the leg says it is).

## 2. Legs 3359, 3360, 3361, 3363, 3401, 3403, 3406, 3409, 3418: `new C[n]` elided at -O2

Repro (3361 shape): `cppon.Cls[] a = new cppon.Cls[2]; int v = a[1].get(); delete a;` then
`cppunw.newArrs()` (the replaced global `operator new[]` in cpp_interop_unwind.cpp) reads 0 at
-O2, 1 at -O0. CFlat emits `call @_Znam(i64)` / `call @_ZdaPv(ptr)` against plain declarations, so
LLVM treats them as the libc++ allocator and removes the allocation pair. clang declares the
replaceable global allocation functions `nobuiltin` and marks new-expression call sites `builtin`;
clang++ -O2 (single TU and -flto) keeps the calls and prints `7 1 1 2`.
Fix direction: mirror clang's declaration/call-site attribute pairing for every replaceable global
allocation function CFlat calls (new, new[], aligned, sized delete forms), then recheck each leg
against a clang++ -O2 oracle.
