# A using-directive hides the nominating namespace's OWN members

Bucket: **p2** (valid C++ refused with a wrong-namespace diagnostic).

## Summary

`ResolveNamespace` rewrites a name through `cxxUsingDirectives_` even when the name ALREADY
names a real namespace. A namespace that carries a using-directive therefore resolves to the
NOMINATED namespace, and its own members stop being findable.

## Repro

`scratch/usingns_c.h`:

```cpp
#pragma once
namespace nsC {
  namespace a { inline int helper3() { return 3; } }
  namespace b { using namespace a; inline int viaUsing3() { return helper3() + 30; } }
}
```

`scratch/usingns_c1.cb`:

```cflat
import cpp "usingns_c.h";
extern int main() { printf("c1=%d\n", nsC.b.viaUsing3()); return 0; }
```

Measured, identical on master 58df69e6 and on branch fix/cpp-using-nested-ns-hang:

```
usingns_c1.cb(2,38): 'viaUsing3' is not a member of namespace 'nsC.a'.   (exit 1)
```

`viaUsing3` is a member of `nsC.b`; the diagnostic even names the namespace the directive
nominated. Reaching a member of the NOMINATED namespace works (`nsC.b.helper3()` prints 3),
so only the nominator's own members are lost.

## Root cause (read, not yet proven under lldb)

`LLVMBackend::ResolveNamespace` (`cflat/LLVMBackend_StateAndImports.cpp` ~1254) consults
`namespaceAliasTable` and then walks `cxxUsingDirectives_` without first checking whether
`name` is itself in `namespaceTable`. With `cxxUsingDirectives_["nsC.b"] = {"nsC.a"}`, the
first candidate `nsC.a` is a registered namespace, so it is returned and the lookup of
`viaUsing3` proceeds in `nsC.a` only.

## Fix direction

Return `name` unchanged when `namespaceTable.count(name) != 0` before the directive walk (a
name that already denotes a namespace is not a candidate for rewriting), or make the member
lookup try the verbatim namespace before the rewritten one and fall back. Either way the
diagnostic must name the namespace the user wrote. Note this is a lookup-ORDER defect, not the
non-termination one fixed on fix/cpp-using-nested-ns-hang.
