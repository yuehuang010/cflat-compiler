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

RULING 2026-09-30 (maintainer), item 1: a USER program may define a function with a C library name (C linkage), even if it replaces the libc one; it must win at every -O level (mark it no-builtin, like clang -fno-builtin-<name>). Core itself must not define externally visible functions - see the core-exported-libc-definitions follow-up.

Attempt (W5, 2026-09-30, item 2): marking global operator new/delete declarations `nobuiltin` and
new/delete-expression call sites `builtin` + allocsize/noalias/nonnull/nounwind (clang's O0 IR
shape) did NOT make the O2 probe match clang (`clang++ -O2` prints `Plain 14 1 1 3`, CFlat exits 3,
allocation effects optimized away). Reverted; patch + matrix in scratch/repro_keep/w5/.

Item 1 LANDED 2026-10-01 as V11 52a77168: a user (non-core) C-linkage definition whose name and
signature TargetLibraryInfo recognizes gets `nobuiltin`; regression leg userLibcAtollWins in
Test/test_operators.cb (-O2 twin). A caller inside an imported .c is folded by clang before
linking (same as clang across TUs without LTO). Item 2 (new[] elision) remains open.

## T51 investigation 2026-10-05 (no code change) - item 2 needs a ruling

CFlat -O2 omitting the replaceable global operator new[] / delete[] calls for `new C[n]` is PERMITTED by C++
([expr.new]/14 allocation omission; [expr.delete]/6.3 then no deallocation call); clang keeping them is also
permitted. Class-scope allocators (leg 3362) keep their counts at -O2; values and destructor counts are preserved.
So the counter legs 3359-3363/3401-3409 encode implementation-defined behaviour: ruling needed - rewrite them to
not count global allocation calls at -O2 (or keep -O0-only), or make CFlat match clang's IR shape anyway.
Separately the full test_cpp_interop.cb at -O2 exits 31 with heap-audit closure/string leaks, NOT localized.
Matrix + report: scratch/repro_keep/t51/.

## Ruling (maintainer, 2026-10-09) - item 2
The optimizer may omit replaceable global operator new[] / delete[] calls ([expr.new]/14). Rewrite
counter legs 3359-3363 / 3401-3409 so they do not count GLOBAL allocation calls at -O2 (class-scope
allocator counts, values and destructor counts stay asserted). No change to CFlat's IR shape.
