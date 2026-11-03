# C++ function pointer returning an array reference loses its C++ alias on direct constructor forwarding

Repro: `using RefFn = char(&(*)())[4]; inline RefFn getfn() { return &buf; }` where `buf` returns a static `char[4]` containing `"abc"`;
then `rv.Fn v = rv.Fn(rv.getfn());`, where `Fn`'s constructor calls `f()[0]`.

Root-cause notes (2026-11-01): on the current worktree exe, the direct constructor form is refused before codegen because the argument
is displayed as `'<c_fn_ptr>'` and no constructor matches. A direct `rv.getfn()()[0]` also loses the array element type and is treated
as indexing `void*`; explicitly casting the returned address to `char*` yields 97. A named `rv.RefFn f = rv.getfn(); rv.Fn v = rv.Fn(f);`
and an inline C++ helper `callfn(rv.getfn())` both return 97. The defect is in forwarding a returned function-pointer value directly
to a constructor while preserving its imported C++ alias/signature; it is not an address loss in the C++ function itself. This item
remains open because the direct form still does not bind after three focused probes.

Pre-existing, seen in the castglob review (2026-11-01, master same): `template<class T> int tref(T& a)` called with a CFlat `int[3]` is
refused (clang 12, T = int[3]); a C++ `int*& p` parameter accepts a CFlat `int[3]` (returns p[0]) where clang refuses the binding.
