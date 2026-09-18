p3

# An rvalue silently binds a NON-const C++ `T&` scalar parameter of a MEMBER function

Found 2026-09-17 while fixing `const T&` scalar binding on the free-function path
(fix/cpp-const-ref-scalar). Measured on that branch's binary AND on master 97773655 - the fix
did not change this cell.

## Repro

```cpp
// header
namespace crs {
    struct Holder { int mem_ncint(int& v) const { v = v + 1; return v; } };
}
```

```cflat
import cpp "crs.h";
extern int main() { crs.Holder h; return h.mem_ncint(41); }   // compiles, returns 42
```

C++ rejects `h.mem_ncint(41)`: a non-const lvalue reference does not bind an rvalue. CFlat
compiles it, materializes a temporary holding 41, lets the callee write 42 into it and then
discards the temporary. Every write through the reference is lost, silently.

The free-function path refuses the same shape (`no overload of 'crs.free_ncint' matches`), and
the constructor path refuses it too (`Test/errors/err_cpp_ctor_ref_literal.cb`), so the member
path is the only one that accepts it.

## Sibling on the FREE path: a storage-less argument SEGFAULTS (review round 1, 2026-09-17)

The free path refuses only the LITERAL spelling. An argument with no storage that still scores
against the pointer parameter reaches the generic `candParamItr->Pointer` branch and passes the
raw scalar, which lowers to `inttoptr` - exit 139, measured identically on master 97773655 and
on fix/cpp-const-ref-scalar (that change keys on `IsCxxConstRef`, so it deliberately leaves the
non-const path alone):

```cflat
inline int nc(int& v) { v = v + 1; return v; }   // C++ side
int a = 40; return rev.nc(a + 1);                // exit 139
return rev.nc(rev.Cst.k);                        // exit 139 (folded constant)
int a = 41; return rev.nc(a);                    // 42, the only correct cell
```

So the accept-set work above must cover the free path too, and the free path's answer for a
storage-less argument is a crash today, not a rejection - that half is arguably p2.

## Root cause

`asAliasIfRef` (`LLVMBackend_CInterop.cpp`, the member-mapping lambda) rewrites every C++ lvalue
reference parameter to the CFlat alias-by-value shape (`Pointer=false, IsAlias=true`), which makes
`ParameterIsAliasByPointer` true, so the argument is lowered through `LowerAliasByPointerArg` -
which materializes a temporary for anything that is not an exact-width lvalue. That is the RIGHT
answer for `const T&` and the wrong one for `T&`. `TypeAndValue::IsCxxConstRef` now records which
one it is (added by fix/cpp-const-ref-scalar and set on this path too), so the information needed
to refuse is present; nothing reads it here yet.

## Fix direction

Refuse an argument that is not an addressable lvalue of the referent's exact type when a member
parameter is a reference and `IsCxxConstRef` is false, with a diagnostic naming the parameter.

This is a NEW rejection on a path that accepts today, so it needs its own accept-set first:
every `T&` member parameter in `core/`, `Test/` and `example/` reached with a converted or
storage-less argument, including libc++ members (`std::vector<T>::push_back` and friends arrive
through the same lambda, some of them via the `T*&` / `IsCxxRefToPointer` branch). Build and
freeze that set as value legs before writing the guard.
