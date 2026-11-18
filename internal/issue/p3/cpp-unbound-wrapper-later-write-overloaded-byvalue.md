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

The selected callee's address is used for a required nontrivial early copy. A re-readable scalar
default is evaluated by a lazy cached helper, so the direct by-value call still supplies the
selected callee's runtime default. The native CFlat callee case below is fixed and covered by
`T518NativeGlobal`.

UNBDEF round 3: a bare trailing CFlat `default` passed to an unbound free wrapper is an explicit
value with the selected parameter type's default value. The wrapper uses a conversion proxy so
clang can select the overload, then constructs the selected parameter type's default. It does not
omit the argument or use a C++ default argument; ambiguity remains clang's located diagnostic.

UNBDEF round 4 accepts a bare trailing `default` only when clang can identify one non-template
free-function candidate by arity, with a named value / `const T&` / `T&&` slot whose type can be
default-constructed. Same-arity overload sets remain a known gap: the bound twin selects by CFlat's
`default` ranking, while the unbound wrapper reports the located known-target-type error.

```cflat
rv2.o1(1, default) // bound rb2.o1(1, default) returns 100
rv2.o2(1, default) // bound rb2.o2(1, default) returns 1
```

Round 3 also confirmed two refused shapes that the bound call accepts:

```cpp
int ld(int, rv::C, int = 4); // rv3.ld(1, default); bound result 104/101
int mx(int, rv::C);          // beside template<class T> mx(T)
                              // rv3.mx(1, default); bound result 70/101
```

Bound wrapper divergences (pinned as current behaviour, not clang; cflat value/stats, clang in brackets):
- bound non-const mixed `nonconst_lref(false ? default : x, ...)`: accepted 12902/10010 [clang refuses]; leg 51901.
- bound overloaded `refs(false ? default : x, ...)`: 20902/10010 [clang 10702/11020]; leg 51903.
- bound overloaded `refs(true ? default : x, ...)`: 20002/20020 [clang 10002/20020]; leg 51904.
- bound overloaded `refs(false ? x : default, ...)`: 20002/20020 [clang 10002/20020]; leg 51905.
- bound rvalue-ref `rref(c ? default : x / x : default, ...)`: refused "takes ownership" [clang 11702/11020, 11002/20020]; err_cpp_unbound_mixed_conditional_nonconst_lref.cb cases 3-4.
