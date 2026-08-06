# A nested emission (generic instantiation, lambda invoker) clears the ENCLOSING function's alias-scope registry

Filed 2026-08-05 from the per-site audit of the `fix/genfp-return` fix. **Pre-existing and
measured IDENTICAL on the pre- and post-fix binaries of that branch** in the exact spelling below
(`--out-lli` diff is byte-identical apart from a one-line offset). Not caused by that fix; found
because it is the SAME defect shape at the SAME boundary, on a different field group.

## The gap

`createFunctionBlock` (`cflat/LLVMBackend.h:4387-4389`) clears the per-function array-view
noalias registry:

```cpp
aliasDomain_ = nullptr;
aliasScopes_.clear();
viewScopeByOrigin_.clear();
```

`BuilderState` / `SaveBuilderState` / `RestoreBuilderState` (`cflat/LLVMBackend.h:9896+`) exist so
that a nested emission mid-body can restore the enclosing function's state. They save the
return-shape group and every owned-temp ledger `createFunctionBlock` clears - **but not these
three.** So emitting a monomorphized generic (or any other nested function) mid-body wipes the
outer function's registry and it is never restored.

The IDs are indices, which is what makes this a correctness problem rather than a missed
optimization: `TypeAndValue::NoaliasScopeId` (`cflat/LLVMBackend.h:695`) is an index into
`aliasScopes_`, minted by `GetOrMintViewScope` and consumed by `AttachViewNoalias`
(`:4345`). Clearing and refilling the vector re-points every ID a live enclosing-function local
is still holding.

## Witness (IR, not a runtime failure yet)

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
function<int(int)> mk<T>(T seed) { return dbl; }
void probe(int n)
{
    int[] a = new int[n];  int[] b = new int[n];
    a[0] = 1;  b[0] = 2;
    function<int(int)> g = mk<int>(1);      // <- nested emission here
    int[] c = new int[n];  int[] d = new int[n];
    c[0] = 3;  d[0] = 4;
    printf("%d\n", a[0] + b[0] + c[0] + d[0] + g(1));
}
```

```llvm
store i32 1, ptr %1, align 4, !alias.scope !0
store i32 2, ptr %3, align 4, !alias.scope !3, !noalias !0
store i32 3, ptr %6, align 4, !alias.scope !5                 ; c: noalias list LOST
store i32 4, ptr %8, align 4, !alias.scope !8, !noalias !5    ; d: disjoint only from c
%9  = load i32, ptr %1, align 4, !alias.scope !10, !noalias !12   ; a: scope !10, not !0
%10 = load i32, ptr %3, align 4, !alias.scope !13, !noalias !15   ; b: scope !13, not !3
```

Two things are wrong at once. `a`'s STORE is tagged scope `!0` and `a`'s LOAD is tagged scope
`!10` - the same view, two scopes. And `c` / `d` are no longer declared disjoint from `a` / `b`.

The control, same program with the `mk<int>(1)` line deleted, is correct on the same binary -
`a`'s store and load both carry `!alias.scope !0`, and `c` carries `!noalias !7` covering the
earlier scopes:

```llvm
store i32 1, ptr %1, align 4, !alias.scope !0
store i32 3, ptr %5, align 4, !alias.scope !5, !noalias !7
%8 = load i32, ptr %1, align 4, !alias.scope !0, !noalias !11
```

## Severity

P2, latent. `!alias.scope` / `!noalias` are a PROMISE to LLVM; a load and a store to the same
memory carrying scopes the metadata declares disjoint permits the optimizer to reorder or drop
one. The probe above prints the right answer today, so there is no runtime witness yet - the same
standing as [[array-view-params-unconditionally-noalias]], which this belongs next to. **Escalate
to P1 the moment a program under `-O2` gives a wrong value from it.**

Note the direction is not uniformly "too much noalias": the lost `!noalias` on `c` is merely
conservative, while the re-pointed scope on `a` is the dangerous half.

## Fix direction

Add `aliasDomain_`, `aliasScopes_` and `viewScopeByOrigin_` to `BuilderState` and save/restore
them alongside the ledgers, exactly as the return-shape group was fixed on `fix/genfp-return`.
Two things to settle first, which is why that branch did NOT fold this in:

1. The nested emission should start with an EMPTY registry (that is what `createFunctionBlock`
   already does) and the outer one should come back untouched - confirm no code depends on a
   scope id crossing the boundary.
2. This changes emitted alias metadata for real programs, so it needs its own `-O2` verification
   pass, not just a green `test.sh`. Neither suite reads alias metadata; a green run says nothing
   about it.

`ResetForReanalysis` already clears all three correctly (`cflat/LLVMBackend.cpp:2430-2432`) with a
comment explaining they are module-bound - that part is not the gap.

## Related

[[array-view-params-unconditionally-noalias]], [[interface-issue-queue]]
