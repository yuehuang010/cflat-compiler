# An indirect call marks a POD `move` argument moved-from; the direct call does not

Filed 2026-08-10 from the `fix/fpmove` accept-set sweep. Measured IDENTICAL on the base `d1b95fe`
and after that fix, so it is PRE-EXISTING and not a regression of it.

Severity: false rejection (a correct program is refused). No memory-safety impact - a POD owns
nothing, so neither spelling frees anything.

## Repro

```cflat
import "function.cb";
int si(move int p) { return p + 1; }
extern int main() {
    int x = 20;
    function<int(move int)> f = si;
    printf("v=%d\n", f(x));
    printf("x=%d\n", x);        // rejected: use of moved variable 'x'
    return 0;
}
```

Measured (`scratch/fp_18_int_after.cb`): rejected on BOTH binaries with
`use of moved variable 'x'`. The direct-call oracle (`scratch/fp_18d_int_after_direct.cb`,
`si(x)` instead of `f(x)`) compiles and prints `v=21`, `x=20` - rc 0 on both binaries.

## Root cause

The indirect call site's own per-param `move` loop
(`MainListener_PostfixExpression.cpp`, after `ApplyFuncPtrSinkTransfer`) calls
`MarkVariableMoved(argNV.CallerName)` for every `FuncPtrParams[i].IsMove`, with no check that the
parameter's type owns a resource. The direct path reaches the same marking only inside
`ApplyMoveParamTransfer`'s transfer block, whose marking is gated on the resolved source type
being a pointer, an owning string, or a struct - a plain `i32` matches none of them, so a POD
`move` argument is never marked at a direct call.

## Fix direction

Delete the ad-hoc loop's storage-nulling and `MarkVariableMoved` half now that
`ApplyFuncPtrSinkTransfer` routes a declared-`move` funcptr param through
`ApplyMoveParamTransfer` (`fix/fpmove`), leaving only the loop's bonded-value diagnostic and
`DiagnoseExplicitMoveToBorrowParam` call. `ApplyMoveParamTransfer` is a strict superset of what
the loop does for every owning shape, and its type gate is exactly the one the POD case needs.
It was left in place by `fix/fpmove` because removing it changes behaviour for arguments whose
`ApplyMoveParamTransfer` guards decline (a borrow source, an interface-borrow arg), and that
accept set was not enumerated in that round.
