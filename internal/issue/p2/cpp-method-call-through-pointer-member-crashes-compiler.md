# A method call through a C++ pointer-typed data member is unsupported, and one spelling crashes the compiler

Found 2026-09-16 by the round-1 review of fix/cpp-ref-member (macOS arm64, Release). PRE-EXISTING:
reproduced on the pre-fix binary with a plain `Foo* f` member; the reference-member fix makes
`Foo& f` reach the same paths.

## Repro

Header:

```cpp
namespace mp {
    struct Foo { int v; int get() const { return v; } };
    struct Holder { Foo* f; Holder(Foo* p) : f(p) {} };
}
```

```cflat
import cpp "mp.h";
extern int main()
{
    mp.Foo foo = default; foo.v = 7;
    mp.Holder h = mp.Holder(&foo);
    if ((*h.f).get() != 7) return 1;    // compiler exits 133 (SIGTRAP), no diagnostic
    return 0;
}
```

`h.f.get()` and `h.f->get()` are both refused with "the function 'f' is not known" (also
pre-existing). The probe used by the review is `scratch/cache_evidence/method_through_ptr_member_p4.cb`
in the main checkout's scratch.

## Root cause

Not established. The deref spelling reaches an LLVM assert / trap instead of a LogError; the dotted
and arrow spellings resolve `f` as a function name rather than a member whose type is a pointer to
a C++ class.

## Fix direction

Route a member whose type is a pointer/reference to a C++ class through the same receiver path as
a C++ class local pointer (`Foo* p = h.f; p->get()` works), for all three spellings; the deref
spelling must at least produce a LogError before the trap (rule: after root-causing an LLVM assert,
add a compiler error for that case). Assert `(*h.f).get()`, `h.f->get()` and `h.f.get()` (if `.`
forwards through the pointer per the operator-dot ruling) in Test/test_cpp_interop.cb.
