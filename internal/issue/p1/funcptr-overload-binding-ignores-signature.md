# A `function<>` argument binds a function-pointer parameter of a DIFFERENT signature and is called

Filed 2026-07-31 by the round-3 review of the closure/interface argument-matching pair
(`4097959`). **Pre-existing**, verified on the master binary at `4097959` - not a regression.

Severity: **SILENT WRONG VALUE, exit 0.** No diagnostic. A function is called through a pointer
whose signature does not match its definition, which is undefined behaviour that happens to
return quietly here.

## Repro

```cflat
import "function.cb";
double dd(double x) { return x + 1000.0; }
interface I { int lam(function<int(int)> f); };
class C : I { int lam(function<int(int)> f) { return f(5); } };
extern int main()
{
    C c; I io = c;
    function<double(double)> g = dd;
    printf("b=%d\n", io.lam(g));
    return 0;
}
```
Observed: prints `b=5`, exit 0. `dd` is invoked as `int(int)`.

The DIRECT path is identical - replace the interface with a plain
`class D { int lam(function<int(int)> f) { return f(5); } };` and `d.lam(g)` also prints `b=5`.
Both paths agree, so this is a scorer gap, not a path divergence.

## Why it is filed now

`4097959` deleted `p1/iface-call-does-no-argument-type-matching.md` after closing that file's
verbatim repro (`io.lam(7)`, an integer into a closure slot). The deletion is earned for the
repro as written, but the deleted file's TITLE was broader than its repro, and this is the part
of that title which is still open. It is recorded separately so the gap does not vanish with
the file.

## Root cause

`ComputeOverloadFunction` compares function-pointer arguments by SHAPE only -
`FunctionPointerShapeOf` (`cflat/LLVMBackend.h` ~16723) reports plain-value / pointer / array,
and the comparison at ~16887 goes no further. The callee and parameter SIGNATURES (return type,
parameter types, arity of the pointed-to function) never participate, so every
`function<...>` matches every other at the same indirection shape.

The gate added by `4097959` does not apply: `ArgumentProvablyMismatchesParameter` takes exactly
one kind of proof - an integer or floating-point VALUE reaching a closure slot - and a
`function<double(double)>` is a genuine function pointer, so it is correctly not proven bad.
Extending that gate is the WRONG place for this; the signature comparison belongs in the scorer.

## Fix direction

Make the encoded closure/function-pointer type name participate in the comparison. The mangled
name already encodes the full signature (`ToUniqueString`, `cflat/LLVMBackend.h` ~713), so an
equality test on the encoded name is likely most of the fix.

**Guard polarity is load-bearing: reject ONLY what you can PROVE.** Two names that differ are
proof of a mismatch and may be rejected; a name that is absent or unresolved is NOT, and must
keep binding as it does today. An earlier revision of `4097959` gated on "the scorer found no
match" and false-rejected every int-to-floating-point interface argument, because scorer silence
is absence of a rule rather than proof - do not repeat that shape.

Watch for the generic case: an unresolved type argument can leave the encoded name incomplete at
scoring time, which must stay permissive.

Verification wants a differential corpus sweep - this touches overload resolution for every
call in every program - plus a check that `Test/test_function_ptr.cb`'s `pickFnPtrShape` /
`pickFnPtrShapeRev` legs still resolve, since they pin the shape ranking added by `4000fa1`.

## Test coverage

None. Wants either a value-asserting leg (if a compatible-signature conversion is defined) or an
`expect_error` leg in `Test/errors/err_data_pointer_to_closure_param.cb`, which is now the home
for closure-argument rejection.

Related: [[data-pointer-into-thin-function-param-segfaults]],
[[funcptr-call-result-into-closure-param-garbage]],
[[funcptr-fixed-array-vs-view-overloads-collide]], [[interface-issue-queue]]
