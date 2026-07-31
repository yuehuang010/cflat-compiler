# A `list<function<>>` element passed to a closure parameter fails the module verifier

Filed 2026-07-31, found while building accept-set parity probes for
`closure-param-accepts-data-pointer`. **Pre-existing**, verified on the master binary at
`4000fa1`.

Severity: hard compile failure with **no source diagnostic**. The only output is an LLVM
module-verifier dump with no `file(line,col):` prefix. Nothing miscompiles.

## Repro

```cflat
import "function.cb";
import "list.cb";
int dbl(int x) { return x * 2; }
class D { int lam(Lambda<int(int)> f) { return f(5); } };
extern int main()
{
    list<function<int(int)>> ls;
    ls.add(dbl);
    D d;
    printf("r=%d\n", d.lam(ls.get(0)));
    return 0;
}
```
Observed on `4000fa1`:
```
Error: module verification failed.
```
exit 1.

## Scope - it is the PASS, not the container

Building the list and invoking the element directly both work. Only passing `ls.get(0)` to a
closure parameter fails. The `list<Lambda<int(int)>>` spelling fails in the same place.

This is why it matters beyond the container: a container element is a legitimate source of a
closure value, and it is the one source that could NOT be exercised when the accept-set parity
table was built for `ce9858e` - it crashes before reaching the gate. So the parity claim for
that fix is verified for every source EXCEPT this one, and closing this issue is what would
let that row be filled in.

## Root cause direction - not diagnosed

Not investigated. Note the neighbouring, already-filed
`p2/generic-wrapper-over-function-type-llvm-fatal` covers a generic STRUCT wrapper over a
function type (`Box<function<>>`) and is likely the same root - a generic instantiated over a
function type mis-lowering its element. Check whether one fix closes both before scoping work;
if so, consolidate on the shared root rather than fixing twice.

## Fix direction

Get the IR (`--out-lli`) and read the verifier's complaint - the failing construct is named in
the dump even though no source location is. Compare the element-load lowering against the
working local-variable spelling.

Per CLAUDE.md, an LLVM-level failure reachable from plain source must become a proper
compile-time error once diagnosed, so a clean `LogErrorContext` is an acceptable outcome if
supporting the shape turns out to be large. But prefer support: a container of callables is an
ordinary thing to want, and both `list<function<>>` and `list<Lambda<>>` construct fine today.

## Test coverage

None. Wants a value-asserting leg once supported, or an `expect_error` leg if rejected.

Related: [[generic-wrapper-over-function-type-llvm-fatal]],
[[data-pointer-into-thin-function-param-segfaults]], [[interface-issue-queue]]
