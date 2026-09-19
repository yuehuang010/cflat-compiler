# Bucket p3. C++ class diagnostics print the MANGLED specialization key

Found 2026-09-19 while fixing
internal/issue/p3/cpp-pointer-to-pointer-element-parameter-unsupported.md (fix/cpp-ptrptr-param).
That commit fixed the four sites in `RejectCxxReferenceFieldStore` /
`RejectInaccessibleCxxMember` (via the new `LLVMBackend::DisplayCxxClassName`, which runs the
identity through `SpellType`). About 30 sibling sites still print the raw key.

## Summary

A C++ class template specialization's CFlat identity is a mangled key
(`std.vector$.p$.p$nest.Cell`), which is not writable source. Messages that format it verbatim
hand the user a name they cannot type.

## Repro

```
import cpp "vector";
import cpp "Test/library/cpp_interop_nest.h";
extern int main()
{
    nest.Cell one = nest.Cell(2, 3);
    nest.Cell* p = &one;
    nest.Cell** pp = &p;
    std.vector<nest.Cell**> v = std.vector<nest.Cell**>(2, pp);
    return 0;
}
```

`C++ class 'std.vector$.p$.p$nest.Cell' has no constructor whose parameter types match these
arguments ('int', 'nest.Cell**')`. Identical on master 33c97f1b and on fix/cpp-ptrptr-param.

## Fix direction

`grep -n "C++ class '{}'" cflat/*.cpp` lists the sites: LLVMBackend_CInterop.cpp (7384, 10312,
10321, 13454, 13525, 14133), LLVMBackend_Overloads.cpp (1794, 2263, 2941, plus the
"has no constructor whose parameter types match" record), MainListener_Declarations.cpp (1761,
3831, 3863, 3881, 3969, 4013, 4074, 4091, 6784), MainListener_PostfixExpression.cpp (6441, 6549),
MainListener_Expressions.cpp (1725, 1766, 1771, 7250, 13267, 13274, 13287, 13302, 13309). Route
each through `DisplayCxxClassName`.

Needs its own accept set: several `Test/errors/err_cpp_*.cb` pin these strings, and
`err_cpp_template_ctor_assignment.cb` pins `std.vector$int` explicitly - it would have to become
`std.vector<int>`. Worth one sweep with a leg per changed err file rather than site-by-site.
