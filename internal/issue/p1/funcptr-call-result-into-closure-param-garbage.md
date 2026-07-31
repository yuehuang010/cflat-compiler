# A `function<>` returned by value and passed to a fat `Lambda<>` parameter yields garbage

Filed 2026-07-31, found while building accept-set parity probes for
`closure-param-accepts-data-pointer`. **Pre-existing**, verified on the master binary at
`4000fa1` and identical on that issue's fix branch.

Severity: **SILENT WRONG VALUE, exit 0.** No diagnostic, no crash, no verifier complaint - the
program runs to completion and prints uninitialized memory. This is the worst failure mode in
the queue: nothing anywhere indicates something went wrong.

## Repro

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
function<int(int)> make() { return dbl; }
class D { int lam(Lambda<int(int)> f) { return f(5); } };
extern int main() { D d; printf("r=%d\n", d.lam(make())); return 0; }
```
Observed on `4000fa1`: prints `r=88133516`, exit 0. Expected `r=10`.

The value **differs between builds** (an earlier build printed `50320968`, another
`44488264`), which is what identifies it as uninitialized memory rather than a stable
mis-computation.

## Scope - the DIRECT path only

The interface path returns the correct `10` for the same shape:

```cflat
interface I { int lam(Lambda<int(int)> f); };
class C : I { int lam(Lambda<int(int)> f) { return f(5); } };
...  I io = c; io.lam(make());     // correct
```

So this is a direct-call lowering defect, not a defect in the closure representation. Every
other source of a thin `function<>` value - local variable, struct field, array element, `?:`
join, `??` join, named function, lambda literal - passes correctly on BOTH paths. A
by-value CALL RESULT is the one that breaks.

## Root cause direction - not diagnosed

Not investigated beyond the observation. The likely area is the direct path's fat-parameter
widen in `CreateOverloadedFunctionCall` (`cflat/LLVMBackend.h`, the arm that now routes through
`WidenToClosureFatChecked` as of `ce9858e`): a call result has no stable storage, so if the
widen reads through a slot that was never populated - or materializes a temporary whose
lifetime ends before the widen - the code slot of the fat `{code, data}` struct would hold
whatever was on the stack.

Confirm by dumping the IR (`--out-lli`) for the repro and comparing the `insertvalue`
sequence against the working local-variable spelling; the difference should be immediate.

Note the provenance gate added in `ce9858e` is NOT implicated - it only decides accept vs
reject, and this shape is correctly accepted on both paths. The bug is in what happens after.

## Test coverage

None. Wants a value-asserting leg (`r=10`) in an existing closure test once fixed - a
compile-only assertion would not have caught this, since it compiles cleanly today.

Related: [[shape-mismatched-funcptr-arg-binds-silently]], [[interface-issue-queue]]
