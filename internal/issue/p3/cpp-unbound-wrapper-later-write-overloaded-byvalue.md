# Unbound C++ wrapper: residue after T518 (unnamed later writes, wrapper move, refused shapes)

T518 landed: through an unbound C++ free wrapper (anonymous-namespace or out-of-scope header
function), a `default` / call-arm `?:` is produced inside the C++ conditional behind a clang
place guard (the selected callee's parameter must be the arm's own class, read from the bound
wrapper - CSigEntry::conditionalArgumentKinds; a conversion target or an all-`default`
conditional is refused, located). A class lvalue argument that a later non-inert argument names:
- trivially copyable: snapshot at its position, named only when clang's SELECTED callee takes it
  by value (CSigEntry::paramBindKinds) - clang-exact;
- otherwise: copy-constructed at its position behind a decision + live flag; when the selected
  callee takes it by value (paramBindKinds 'v') the call bypasses the wrapper and calls the
  selected callee through its address (generated `__cflat_byval_<hash>` signature + `_fp` getter,
  CSigEntry::selectedCalleeType / selectedCalleeDefaults) on the M4b by-value ABI path, eliding
  into that copy (a `c ? default : x` join elides into the default temp or the copy). One
  construction, in evaluation order, ctor/dtor counts equal clang. A reference selection keeps
  the caller's object and constructs nothing.

The wrapper is refused (located) when the selected callee's address cannot be taken or another
argument can only be built by the wrapper (including a non-constant omitted default, a variadic
extra argument, deferred `?:`, or direct construction). Global writes, implicit-this writes,
and writes through a returned pointer can instead fall back to the wrapper and read the
post-write value; a native CFlat callee reads a later-written global after the write.

Still open:
- A global written later into a callee that refuses falls back to the wrapper and reads the
  post-write value (g_var_write, g_var_named; cflat 90201010, clang 70201010).
- An implicit-this method write is not snapshotted (t_self_meth; cflat 90221030, clang 70221030).
- A write through a returned pointer is not snapshotted (t_chain_ret; cflat 90221030, clang 70221030).
- A global written by a later argument of a native CFlat callee is read after the write
  (g_native_write `nlater(rv.gv, rv.mutglobal())`; cflat 90201010, clang 70201010; counts match).
- A `?:` with a `default` arm into a REFERENCE parameter: C++ materializes the leaf's copy at the
  argument, the wrapper binds the caller's object (lref ternary 902 on master and fix, clang 702).
- `take2(c ? default : rettw(), default)`: a bare trailing `default` into an unbound wrapper has no
  target type and stays refused.

The remaining conditional-reference repro is retained at scratch/unbwrap/item3.cb.
