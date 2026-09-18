Bucket: p3

# A GLOBAL-SCOPE C++ class template is not nameable from CFlat, at any spelling
(compile failure on a legal header spelling, no silent wrong data).

## Summary

Narrowed and re-measured 2026-09-17 while fixing
`p2/cpp-alias-template-fixed-nontype-argument-dropped.md`. That fix records an alias template's
target STRUCTURALLY (clang's qualified name for the target decl), so the half of this issue about
a target in the ALIAS'S OWN namespace is gone:
`namespace alnp { template<class T> struct ACell{}; template<class T> using ACellSelf = ACell<T>; }`
now resolves (leg 2218 in `Test/test_cpp_interop_template.cb`).

What remains is NOT an alias defect. A C++ class template declared at GLOBAL scope cannot be
named from CFlat even with no alias involved:

```cflat
import cpp "cpp_interop_tpl.h";
extern int main() { GlobalBox<int> c = default; return 0; }   // cannot find the type 'GlobalBox<int>'
```

`GlobalBox` is `template <class T> struct GlobalBox { T value; };` at global scope in
`Test/library/cpp_interop_tpl.h` (line 1442). A global-scope alias TO it
(`template<class T> using GAlnBoxAlias = GAlnBox<T>;`) fails the same way and for the same
reason: the target has no binding to hop to. A global-scope alias whose target is NAMESPACED
works and keeps its fixed argument (leg 2214).

## Repro

`scratch/aln_c10f.cb` (direct, no alias) and `scratch/aln_c11_global_unqual.cb` (via alias) in
the fix-cpp-alias-nontype worktree; both refused on master 884a0c08 and on the branch.

## Root cause

Not established. The binding path registers C++ class templates that the use site reaches
through a namespace; a global-scope template appears not to be registered under a key any CFlat
spelling produces. A concrete C++ specialization of a global template DOES bind
(`using GlobalBoxAlias = GlobalBox<int>;` works), so the gap is in naming the TEMPLATE, not the
record.

## Fix direction

Find where a C++ class template is registered as a generic base and why a global-scope one gets
no key a bare CFlat spelling finds. Needs its own accept-set: a bare spelling must not start
absorbing a same-named CFlat generic.
