# A function-pointer VALUE still converts to a data pointer at initialization, assignment and return

Filed 2026-08-03 by review round 1 of `fix/funcptr-rebind`, which closed the OVERLOAD-RESOLUTION
path of this defect class and left these.

Severity: **P1 - memory-unsafe, silent.** A code address is stored in a data pointer and written
through.

## Repros - all exit 138, no diagnostic, identical on `904f026` and on `fix/funcptr-rebind`

Declaration-init (`scratch/rev_v_assign.cb`):

```cflat
import "function.cb";
struct Rec { int a = default; int b = default; };
double ro(double x) { return x + 1000.0; }
extern int main() { function<double(double)> w = ro; Rec* r = w; r->a = 11; printf("a=%d\n", r->a); return 0; }
```

Return (`scratch/rev_v_ret.cb`):

```cflat
Rec* give() { function<double(double)> w = ro; return w; }
```

Field store (`scratch/fpr_v_field.cb`): `struct Box { Rec* p = default; }` then `b.p = w;`.

## Root cause

The gate landed by `fix/funcptr-rebind` lives in `ComputeOverloadFunction` (`cflat/LLVMBackend.h`)
and in the variadic short-circuit next to it - i.e. it is a rule about ARGUMENT BINDING only. The
conversion sites reached by a declarator initializer, an assignment, a `return`, and a field store
are separate lowering paths, and under opaque pointers each of them sees a code pointer and a data
pointer as one `ptr`.

## Fix direction

The predicates already exist and are already shared: `ArgumentIsCodeValue` (shape 0 plus
`ArgumentIsFunctionPointerish`) and `ParameterStoresData`. What is missing is a DESTINATION-side
reader for the non-argument conversion sites.

Scoping the fix to the scorer was deliberate, not an oversight: the scorer is one funnel with a
measured accept set, while the store paths are several sites and a rejection there is the shape
that has repeatedly false-rejected working code in this repo. Build the accept set first - an
explicit `(Rec*)` cast must keep working, and so must the `function<T>*` / `function<T>[N]` shapes
that are genuinely data.

Distinct from `p2/data-pointer-returned-as-closure-not-gated`, which is the MIRROR direction (a
data pointer flowing INTO a callable), and from the mirror argument leg still open in the funcptr
parameter arm.
