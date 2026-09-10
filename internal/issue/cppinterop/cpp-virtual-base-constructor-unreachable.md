# A C++ class with virtual bases has no constructor cflat can call

Found 2026-09-10 while landing the virtual-member thunk (see
[`std-header-coverage-spike.md`](std-header-coverage-spike.md) gap 3). Diagnosed and GUARDED
there, not fixed.

## Repro

```cflat
import cpp "cpp_interop_poly.h";
extern int main() { cpppoly.VBaseUse<int> v = default; return 0; }
```

    C++ class 'cpppoly.VBaseUse$int' has no default constructor cflat can call - initialize it
    with 'cpppoly.VBaseUse$int(args)'

`--check -v` names the reason:

    C++ member cpppoly.VBaseUse$int.__ctor not bound: belongs to a class with virtual bases,
    whose constructor takes an implicit most-derived argument cflat does not pass

`std.basic_ofstream<char>` and `std.basic_ostream<char, std.char_traits<char>>` hit the same
refusal, which is what still keeps a stream out of a local.

## Root cause

The complete-object constructor of a class with virtual bases takes an IMPLICIT extra argument in
every ABI cflat targets: the MS is-most-derived flag, the Itanium VTT pointer. `arrangeCXXMethodType`
(what `ComputeCxxMemberAbi` calls) arranges the DECLARED prototype and does not include it, so the
mismatch is invisible to the `paramTypes.size() != abi.params.size()` guard, and cflat emitted a
call with the flag register left holding whatever was in it.

Measured before the guard went in, the exact `msp::VDer<int>* p = new msp::VDer<int>();` probe was
stopped earlier by the existing no-callable-destructor refusal. The same constructor hazard is
reproduced by `msp::VBoth<int> vb = default;`, which generated an executable that crashed with an
access violation; a virtual-base class with a trivial destructor (`msp::PlainVDer<int>`) instead
failed to link on its unresolved vbase destructor. Neither is a usable path, so the guard narrows
nothing that worked.

Only a TEMPLATE specialization reaches this at all: a directly named class with virtual bases is
refused at layout first (`err_cpp_virtual_base.cb`).

## Fix direction

Arrange the constructor with `arrangeCXXConstructorCall` (or `arrangeCXXStructorDeclaration`) so
the implicit argument is part of the plan, and pass 1 for a complete object at the call site.
Alternatively route construction through the same synthesized-thunk mechanism the destructor now
uses - a `static void __cflat_vctor_<tag>(void* m, ...) { ::new (m) T(...); }` wrapper lets clang
supply the argument, exactly as `__cflat_use_ctor` already does for instantiation.

Acceptance: `cpppoly.VBaseUse<int> v = default;` constructs, `v.t()` returns 107, and the
destructor runs exactly once at scope exit, asserted in Section M41 of
`Test/test_cpp_interop.cb`. The `expect_error` leg in `Test/errors/err_cpp_virtual_base.cb` that
pins today's refusal must be removed in the same change.
