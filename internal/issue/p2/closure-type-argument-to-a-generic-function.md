# A closure type as a GENERIC FUNCTION's type argument does not resolve

Filed 2026-08-05 out of the Phase A coverage matrix for
`lambda-pointer-as-generic-type-arg-bypasses-guard`. Measured IDENTICAL on the pre-fix binary
(master `4c06cce`) and the post-fix binary - a neighbouring gap, not a regression.

A closure type works as the type argument of a generic STRUCT (`Box<Lambda<int(int)>>`,
`list<function<int(int)>>`), and after the pointer fix `Box<function<int(int)>*>` works too.
The same argument to a generic FUNCTION does not resolve at all, in either the explicit or the
inferred spelling.

## Repro - explicit type argument

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
T idf<T>(T v) { return v; }
extern int main() {
    function<int(int)> g = dbl;
    function<int(int)> r = idf<function<int(int)>>(g);
    printf("%d\n", r(6)); return 0; }
```
Both binaries: `(3,9): unknown type 'function<int(int)>'` - reported on the TEMPLATE's parameter
list, not on the call. The `Lambda<...>` spelling and the `<function<int(int)>*>` spelling fail
the same way.

## Repro - inferred type argument

```cflat
    function<int(int)> r = idf(g);
```
Both binaries:
```
(6,27): no overload of 'idf__i8' matches the given arguments.
  Call arguments (1):
    [0] ptr <unnamed>
  Candidates (1):
    _idf__i8_i8_i8_(i8 v)
```
The instantiation was keyed `idf__i8` - the closure argument mangled as a bare `i8` rather than
through the encoded closure name (`__thinfn_1_3_i32_3_i32`).

The non-closure control works on both binaries: `int* r = idf<int*>(&n);` prints `7`.

## Root cause (hypothesis, not yet measured)

`MainListener::ResolveTypeArgEntry` is the funnel that encodes a closure type argument
(`functionPointerSpecifier` branch -> `EncodeClosureCodegen`) and every generic STRUCT path
reaches it. The generic-FUNCTION type-argument path appears not to - the explicit form keeps the
raw source text `function<int(int)>` as a type name, and the inferred form mangles the argument
from its LLVM repr (`ptr` -> `i8`) instead of from its CFlat type. Both are consistent with the
call-site type-argument list being resolved somewhere other than `ResolveTypeArgEntry`.

Verify that before fixing: the filed root cause of the issue this came out of was itself a
hypothesis with a citation, and the repro had drifted from what was recorded.

## Fix direction

Route the generic-function type-argument list (explicit and inferred) through
`ResolveTypeArgEntry` / the same encode-and-register funnel the struct path uses, so a closure
argument mangles to its encoded name in both. The fat/thin pointer asymmetry landed for struct
arguments then applies unchanged, since it lives in that funnel.

Related: [[interface-issue-queue]], [[closure-by-value-into-generic-struct-field]]
