# Interface-method dispatch never gates a code-value argument (memory-unsafe accept)

Filed 2026-08-08 by review round 1 of `fix/cast-launder-occurrence`. Pre-existing hole,
unrelated to the occurrence-keying mechanism that fix landed: the code-value gate never runs
on this call path at all, cast or no cast, so occurrence keying cannot be the fix here.

**Severity: silent memory-unsafe accept, exit 138.**

## Repro (`scratch/rev/rev_p10_iface_method.cb`, `scratch/rev/rev_p12_iface_nocast.cb`)

```cflat
import "function.cb";
struct Rec { int a = default; int b = default; };
double ro(double x) { return x + 1000.0; }
interface ITake { int take(void* v, Rec* p); };
class Taker : ITake { int take(void* v, Rec* p) { p->a = 7; return p->a; } };
extern int main(int argc, char** argv)
{
    Rec* n = nullptr;
    Taker* t = new Taker();
    ITake i = t;
    printf("%d\n", i.take((void*)ro, argc > 0 ? ro : n));  // compiles clean -> exit 138
    return 0;
}
```

`rev_p12_iface_nocast.cb` drops the cast entirely (`i.take(nullptr, argc > 0 ? ro : n)`) and
reproduces identically - proving the gate is not merely laundered here, it is never consulted:
a direct (non-interface) call with the same shapes IS diagnosed
(`Test/errors/err_data_pointer_to_closure_param.cb`), so the discriminator is the interface
dispatch path itself, not the join or the cast.

## Root cause

Not yet fully investigated. The interface-method call's argument-evaluation loop in
`MainListener_PostfixExpression.cpp` (the `extraArgs`-building loop, distinct from the
direct/member call loop) does now scope a cast occurrence per argument (added by
`fix/cast-launder-occurrence`, so a cast there no longer launders a SIBLING argument), but
that loop's resulting `NamedVariable`s are apparently never run through
`CodeValueIntoDataDestination`/`ArgumentIsCodeValue` against the interface method's DECLARED
parameter types the way the direct-call overload-resolution path (`LLVMBackend_Overloads.cpp`)
does. Interface dispatch binds through the vtable slot's declared signature rather than
overload resolution, so the code-value gate - wired into the overload scorer and the
non-interface direct-call check - has no equivalent hook on this path.

## Fix direction

Not investigated. Likely needs an explicit `CodeValueIntoDataDestination`-style check inserted
into the interface-dispatch argument-binding code, parallel to what
`LLVMBackend_Overloads.cpp` does for ordinary overload resolution, rather than relying on the
scorer (which interface dispatch does not go through).

Related: [[interface-issue-queue]]
