# An inferred owning sink is lost when a closure crosses a declared boundary

Filed 2026-08-09 by the work on `fix/lamsink`, which fixed the lambda-literal half of
`p1/lambda-parameters-get-no-owning-sink-inference.md`. These cells are the residue: measured
IDENTICAL on the base `a6f7373` and after that fix (rc 133 both), so not a regression of it.

Severity: double free (abort, rc 133).

## Repro

```cflat
import "function.cb";
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

void call(Lambda<void(UBox)> g, UBox x) { g(x); }

extern int main() {
    UBox a = umk(5);
    call((UBox p) => { UBox[2] d = { p }; printf("elem=%d\n", d[0].item->id); }, a);
    printf("dtor=%d\n", dtor); return 0;
}
```

Measured (`scratch/ls_b2_passed_as_argument.cb`): prints `elem=5`, frees in the lambda body, then
aborts on `call`'s `x` / the caller's `a` - rc 133, both binaries.

Three more spellings of the same shape, all rc 133 on both binaries:

- `ls_b3_named_then_arg.cb` - the closure is bound to a `Lambda<void(UBox)>` local first, then
  passed to `call`.
- `ls_d10_generic_call_helper.cb` - `void callg<T>(Lambda<void(T)> g, T x) { g(x); }`.
- `ls_d3_returned_lambda.cb` - the literal is returned through a declared
  `Lambda<void(UBox)>` return type and invoked through the returned value.
- `ls_d9_lambda_in_struct_field.cb` - the literal is stored into a `Lambda<void(UBox)>` STRUCT
  FIELD (`h.f = (UBox p) => { ... };`) and invoked as `h.f(a)`.

The direct-function spelling of the same body is rc 0 (`ls_b5_function_oracle.cb`).

## Root cause

`fix/lamsink` runs `ApplyOwningSinkInference` over a lambda literal's parameter list and carries
the result on the TYPE (`TypeAndValue::FuncPtrParam::IsOwningSink` /
`IsConsumeInferredSink`), so the indirect call site can transfer ownership. A declared
`Lambda<...>` / `function<...>` spelling cannot state an inferred sink, so the flags are adopted
from the initializer at ONE door only: the declarator initializer
(`MainListener_Declarations.cpp`, the `AdoptInferredParamSinks` call next to the per-param
`IsMove` agreement check).

The remaining doors have no adoption:

- a PARAMETER of closure type - there is no initializer at all, and the caller's argument type is
  not visible where the callee's parameter type is registered;
- a `return` through a declared closure return type;
- a struct FIELD of closure type (`=` assignment; adopting onto `StructData`'s field type would
  make the claim type-wide, across instances, which was judged too broad to do blind);
- plain `=` re-assignment to an existing closure local.

The callee half already consumes on all of them (the element/slot consume arms are shared), so
the caller half is the only missing piece - exactly the shape the fixed issue had.

## Fix direction

Two candidate directions, neither free:

1. Extend adoption to the remaining doors. The `=` assignment door needs a way to update the
   REGISTERED variable's `TypeAndValue` (not the local `NamedVariable` copy). The field door
   needs a per-instance answer or an accepted type-wide over-approximation. The parameter and
   return doors need the fact to travel with the VALUE, which the closure fat struct does not
   carry - so they likely cannot be closed this way at all.
2. Make the sink part of what a closure type can SPELL, i.e. require
   `Lambda<void(move UBox)>` for a consuming lambda and reject the un-spelled form at the
   crossing. Note the `move` spelling currently needs BOTH sides to state it and the literal
   has no way to (`ls_e1_move_spelled.cb`: `Lambda<void(move UBox)> f = (UBox p) => ...` is
   "incompatible function pointer initializer: parameter 1 differs in 'move' modifier" on both
   binaries), so this direction needs a lambda-parameter `move` spelling first.

Do NOT reach for a blanket rejection of consuming lambda bodies: `f(umk(5))` with a TEMP argument
is rc 0 on both binaries (`ls_e2_rvalue_arg.cb`) and the direct-function twin is rc 0 too, so a
rejection keyed on the body alone would refuse programs that work.
