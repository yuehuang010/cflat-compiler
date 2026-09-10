# An alias naming a class-template specialization resolves as a type but loses its fields

Found 2026-09-09 as cell `c05` of the coverage matrix for the `std` namespace-member fix
(`fix/std-namespace-members`). Recorded, not fixed there - it is a different mechanism
(`cxxLazyAliasSpecializations_`, the path section M21 of `Test/test_cpp_interop.cb` claims to
cover) from the one that fix touched. Measured IDENTICAL on both the pre- and post-fix binaries,
so it is pre-existing and independent.

## Repro

Header (`alias_tpl.h`):

```cpp
namespace refns {
    template <class T> struct RefTpl { T v; };
    using RefTplAlias = RefTpl<int>;
}
```

CFlat:

```cflat
import cpp "alias_tpl.h";
extern int main() { refns.RefTplAlias b = default; b.v = 5; if (b.v != 5) return 1; return 0; }
```

    Unknown identifier 'v'.

The alias resolves far enough to DECLARE the local - the failure is on field access, so a partial
registration is landing: the type exists, its fields do not.

## Sibling worth checking in the same work

Matrix cell `b07`: an alias spelled as a template ARGUMENT (`std.vector<std.ofstream>`) goes
through the EAGER pre-request spelling path rather than the lazy type request, and was also
measured identical on both binaries. Different entry point, plausibly the same underlying
half-registration. Probe it before assuming it is separate.

## Root cause

Not established. Start at `cxxLazyAliasSpecializations_` and at whatever section M21 exercises -
M21 claims "lazy aliases" coverage and this case defeats it, so either M21's fixture does not
reach the specialization sub-case or the registration is incomplete for it. Establish which before
editing.

## Fix direction

Make an alias of a class-template specialization register the same record the specialization's own
spelling registers - fields included. Confirm by declaring the local through BOTH spellings
(`refns.RefTplAlias` and `refns.RefTpl<int>`) and asserting they reach the same record.

Acceptance: the repro compiles and runs to exit 0; both spellings asserted against the same field
value in `Test/test_cpp_interop.cb`, extending the M21 section rather than adding a new file.
