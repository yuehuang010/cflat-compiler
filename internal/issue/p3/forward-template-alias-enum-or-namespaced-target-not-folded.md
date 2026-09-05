# Forward template use with an enum-pointer or namespaced pointer-alias argument still says "cannot find the type"

Bucket: batch mode (diagnostic only). Filed 2026-09-04 by the q16 review (9ab7d502), which
folded SIMPLE pointer aliases (`using IP = int*;`) in the scanner's forward template-use encoder
through the mangling-only `manglingPointerAliases_` table.

## Repro

```
enum E { A, B };  using EP = E*;
struct H { V<EP> f = default; };      // template used before its body
namespace N { struct T { int x; }; }  using NP = N.T*;
struct H2 { V<NP> g = default; };
struct V<T> { T v = default; };
```

Both still report `cannot find the type 'V<...>'` instead of the truthful incomplete-by-value
message (`V<int*>` gets it since q16). Unchanged PRE/POST across q16.

## Root cause

`ForwardRefScanner::RegisterRenameAlias` (cflat/ForwardRefScanner.cpp ~843) pre-registers a
pointer alias only when the bare target is `IsKnownTypeName` or in `gts.scannedTypeNames`;
`recordTypeName` fires only for struct/class/interface, so enums are never in that set. The
namespaced target passes `IsBareTypeName` and namespaces are descended by the collector, so its
rejection is elsewhere in the guard chain - localize with an instrumented build before fixing.

## Fix direction

Record enum names in the scanned set (check every other consumer of `scannedTypeNames` first -
it is the type-argument accept set for the whole compiler) or consult the enum table in the
guard; for the namespaced case find the failing predicate. Legs: extend
Test/errors/err_unknown_type_arg_qualifier.cb (q16 legs live there) and testQ16ForwardAcceptSet
in Test/test_generics.cb. Both ParseDeclarationSpecifiers copies untouched; the fold stays in
`FoldPointerAliasArg` (single definition).
