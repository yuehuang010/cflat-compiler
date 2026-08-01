# A `function<>` argument binds a function-pointer parameter of a DIFFERENT signature and is called

Filed 2026-07-31 by the round-3 review of the closure/interface argument-matching pair
(`4097959`). **NARROWED 2026-07-31** by `fix/funcptr-arg-accept-set`, which closed the CLASS axis
of this title and left the rest open. Do not re-close it on the strength of the original repro:
that repro is fixed, and this file is now about the part of the title that is not.

Severity: **P1 - SILENT WRONG VALUE, exit 0, and one memory-unsafe case.** No diagnostic.
Kept at P1 rather than dropped to P2 because `u2` below is an out-of-bounds READ through a
wrong-pointee pointer, not merely a wrong number, and because every case is silent - a wrong
value with no diagnostic is the failure mode this queue ranks worst. It is NOT a regression:
every repro below behaves identically on master and on the fix.

## What was closed

`fix/funcptr-arg-accept-set` made the function-pointer SIGNATURE participate in binding on both
the direct and the virtual path. Argument assembly now propagates the signature fields onto the
call argument (previously they never reached the scorer at all - the original file blamed the
scorer for "comparing shape only", but the scorer could not see the signature in the first
place), and two proof sites compare it: the funcptr clause of `ComputeOverloadFunction` and
`ArgumentProvablyMismatchesParameter` (which the interface lone-slot arm uses).

The comparison is on coarse TYPE CLASSES (`FuncPtrTypeClass`): `i` integer, `f` floating point,
`p` pointer, `v` void, unknown for a struct / interface / unsubstituted generic. The original
repro - `function<double(double)>` into a `function<int(int)>` slot - is `f` vs `i` and is now
rejected on both paths.

## What is still open, with repros

The classes are coarse by design, so a mismatch WITHIN a class is invisible. All of these compile
and run, silently, on master and on the fix alike. Each repro is reproduced verbatim below.

**u4 - floating-point WIDTH. The decisive one: same SHAPE as the closed repro.**

```cflat
import "function.cb";
float ff(float x) { return x + 1.5f; }
int callD(function<double(double)> f) { return (int)f(2.0); }
int callD(int n) { return 200 + n; }
extern int main() { function<float(float)> g = ff; printf("d=%d\n", callD(g)); return 0; }
```
Prints `d=0`. A floating-point callback bound to a floating-point slot of a different type - the
closed repro differed only in that `int` and `double` land in different classes.

**u2 - POINTEE type. Memory-unsafe: reads past the end of the object.**

```cflat
import "function.cb";
struct Circle { int r; double area; };
struct Square { int side; };
void onCircle(Circle* c) { printf("circle r=%d area=%f\n", c->r, c->area); }
int callSq(function<void(Square*)> f) { Square s; s.side = 77; f(&s); return 7; }
int callSq(double d) { return 200 + (int)d; }
extern int main() { function<void(Circle*)> g = onCircle; printf("r=%d\n", callSq(g)); return 0; }
```
`onCircle` reads `c->area` out of a 4-byte `Square`. Prints `circle r=77 area=0.000000`, exit 0.
"Any pointer is one class" makes this permanently invisible to the current mechanism, and that
choice is deliberate - see the fix direction.

**u1 - integer WIDTH.**

```cflat
import "function.cb";
i8 narrow(i8 x) { return (i8)(x + 1); }
int callWide(function<i64(i64)> f) { return (int)f(5000000000); }
int callWide(double d) { return 200 + (int)d; }
extern int main() { function<i8(i8)> g = narrow; printf("wide=%d\n", callWide(g)); return 0; }
```
Prints `wide=705032705`.

Also open on the same axis, not repeated in full: SIGNEDNESS (`function<u64(u64)>` into a
`function<i32(i32)>` slot prints `s=-3`), and AGGREGATES (`function<void(Pt)>` into a
`function<void(int)>` slot passes the struct as an int; a struct component is class-unknown, so
the whole signature stops being comparable).

## Second residual: a rejected candidate REBINDS instead of erroring

`result = -1` in `ComputeOverloadFunction` is a PREFERENCE verdict - "this candidate does not
match" - not a validation one. When a same-arity sibling can absorb a pointer, the call silently
binds to the sibling rather than becoming an error:

```cflat
int lam(function<int(int)> f) { return f(5); }
int lam(void* p)              { return 999; }   // takes the call for a mismatched signature
```
Master printed `b=5` (calling the callee at the wrong type); the fix prints `b=999`. Neither is
right. Same with a variadic sibling (`int lam(char* fmt, ...)`) and with an interface slot pair
(`int lam(function<int(int)> f); int lam(void* p);`). Pinned as a recorded gap by the
`rebindProbe` legs in `Test/test_function_ptr.cb`, which also assert that a MATCHING signature
still beats the pointer-absorbing sibling.

This is not proposed as a rejection in its own right: a candidate that does not match yielding to
one that does is how overload resolution works everywhere. What is wrong is narrower - that a
`function<>` VALUE is silently absorbed by a `void*` / variadic parameter at all - and it is
pre-existing, reachable with no signature mismatch anywhere. It is recorded here rather than in
its own file because the real fix below closes both.

## Fix direction

**Do NOT close this by widening the type classes to compare more of the spelling.** That was
tried and it is a false-rejection engine. Round 1 of the fix compared canonicalized type NAMES
(aliases and enums resolved) and hard-errored on six programs master runs correctly -
`function<int(int)>` into a `function<i32(i32)>` slot, `long`/`i64`, `int*`/`void*`,
`char*`/`string` - on both proof sites and both paths, including ordinary single-candidate calls.
`int` and `i32` are ONE type spelled two ways; the scorer says so itself forty lines below
(`int==i32`), and `IsKnownTypeName` puts them in one scalar set. In-repo `.cb` files were green
only because none happens to cross a spelling boundary at a `function<>` argument, and a
whole-corpus differential sweep structurally cannot see a crossing no corpus file performs.

The correct proof is the callee's actual lowered `llvm::FunctionType` at the argument, compared
against the parameter's lowered function type. That is exactly what the scorer does not have: a
call-site argument carries a `TypeAndValue` whose funcptr fields are declared SPELLINGS, and
under opaque pointers the LLVM value is an indistinguishable `ptr` - the lowered callee type is
recoverable only where the argument's definition is (a named function, a `function<>` variable's
declared type). Any fix has to carry the lowered type from the declaration site to the argument,
or resolve the callee at the argument the way `GetFunctionForFuncPtr` does for the `move`-flag
check - and then it must still be one-sided, rejecting only where BOTH lowered types are known
and differ.

## Test coverage

The CLASS axis is covered on both paths by the two signature legs in
`Test/errors/err_data_pointer_to_closure_param.cb` (each with two same-arity candidates, so the
scorer is what they test). The rebind residual is pinned in `Test/test_function_ptr.cb`. The
axes above have NO coverage - they compile and run silently, so an `expect_error` leg cannot be
written until there is an error to expect.

Related: [[shape-mismatched-funcptr-arg-binds-silently]],
[[funcptr-fixed-array-vs-view-overloads-collide]], [[interface-issue-queue]]
