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

Refused (located) where that call is unavailable: a non-constant default argument the call
omits, a variadic callee given extra arguments, another argument that only the wrapper can build
(deferred `?:` / direct construction), a selected callee whose address cannot be taken.

Still open:
- A later argument that writes the object without naming its local/parameter root is not
  detected: an implicit-this field or a global reads 902 where clang reads 702. Same on master.
- Wrapper cost (not ternary-specific): a by-value C++ class argument moves once more through the
  CFlat wrapper (`take(rettw())`, `take(default)` 1010020 vs clang 1000010).
- A `?:` with a `default` arm into a REFERENCE parameter: C++ materializes the leaf's copy at the
  argument, the wrapper binds the caller's object (lref ternary 902 on master and fix, clang 702).
- `take2(c ? default : rettw(), default)`: a bare trailing `default` into an unbound wrapper has no
  target type and stays refused.

Repro: scratch/rev_t518_sol_tmp/{name_field,name_global}.cb in the T518 worktree.
