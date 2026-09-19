# Bucket p2. A C++ `T *const &` member parameter or return keeps the RAW `T**` shape

Found 2026-09-19 while fixing
internal/issue/p3/cpp-pointer-to-pointer-element-parameter-unsupported.md (fix/cpp-ptrptr-param,
macOS arm64, Release). Measured identical on master 33c97f1b and on that branch, in the same
spelling - the ptrptr fix does not touch it.

## Summary

Re-bucketed p3 -> p2 in review 2026-09-19: a `Cell *const &cref_out() const` return binds as
`Cell*` but holds the ADDRESS of the pointer; dereferencing it segfaults (exit 139, branch and
master) from a clean compile with no diagnostic - memory safety, not a wrong number.

`CxxSpellingWithoutRef` strips trailing `&` but NOT the trailing cv word underneath it, so
`nest::Cell *const &` comes back as `nest::Cell *const`. `asAliasIfRef` then tests
`bare.back() == '*'`, which fails, and the parameter/return keeps the raw extra-pointer shape
instead of becoming an alias. The non-const twin `nest::Cell *&` takes the alias path and is
correct. `std.vector<T*>::push_back` only works because of a name-keyed special case
(`r.name.starts_with("std.vector$") && m.name == "push_back"` in LLVMBackend_CInterop.cpp).

## Repro

scratch/ppp_e_base.h + scratch/ppp_f_ret1.cb in the fix worktree:

```
struct H1 {
    nest::Cell *s;
    int take(nest::Cell *const &p) { s = p; return p->b * 10 + 5; }
    nest::Cell *&ref_out() { return s; }
    nest::Cell *const &cref_out() const { return s; }
};
```

- `h.take(p)` is rejected: "parameter 1 'p' has type 'nest.Cell**' and the argument has type
  'nest.Cell*'". `h.take(&p)` compiles and is correct, so the surface leaks the ABI level.
- `h.ref_out()` reads back 3 (correct). `h.cref_out()` reads back 1 - a WRONG VALUE, silently,
  because the result is the slot address typed as `nest.Cell*`.

## Fix direction

Strip trailing cv words in `CxxSpellingWithoutRef` before classifying `RefToPointer` and before
returning the bare spelling. Needs its own accept set: the surface type of every `T *const &`
member parameter changes (call sites that today pass `&x` would stop compiling), and the
std.vector push_back special case should then be removable. Wants a maintainer ruling on whether
the `&x` spelling stays accepted.
