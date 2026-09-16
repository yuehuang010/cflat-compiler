# A C++ namespace alias binds nothing

## Summary

`namespace short_name = some::long::name;` in an imported C++ header is harvested for nothing -
not variables, not functions, not types. Every use through the alias is `Undefined variable`.

## Repro

```c
// header
namespace nsv { int twice(int x); extern int counter; }
namespace nsalias = nsv;
```

```c
import cpp "h.h";
extern int main() { return nsalias.twice(5); }   // Undefined variable nsalias.
extern int main() { return nsalias.counter; }    // Undefined variable nsalias.
```

Measured 2026-09-16 on master and on the namespace-variable branch: identical both sides, so this
is not specific to objects. `nsv.twice(5)` and `nsv.counter` both work.

## Root cause

Not investigated. `NamespaceAliasDecl` is not visited in cflat/CClangExtract.cpp; only
`usingDirectives` (namespace-scope `using namespace X;`) is harvested, as a lookup-time fallback
pair. An alias is the same shape of information - alias name -> nominated namespace - and the
existing lookup fallback may be enough to carry it.

## Fix direction

Harvest `NamespaceAliasDecl` into a dotted (alias, target) pair alongside `usingDirectives`,
persist it in the header disk cache (bump the version), and resolve the alias prefix at lookup
time before the namespace-member error fires. Coverage: alias to a top-level namespace, to a
nested one, an alias of an alias, reaching a function, a type, an enum member and a bound
namespace-scope object.
