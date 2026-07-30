# A method call on a default/null interface value SIGSEGVs - no null guard

Filed 2026-07-29 out of round 4 of the adversarial review of
`generic-interface-registered-as-opaque-struct.md`. **Pre-existing and language-wide** - it is NOT
specific to generic interfaces and is NOT caused by that change. Verified against the pre-fix
binary.

## Repro - a plain NON-GENERIC interface is enough

`scratch/rev4/p/ctl02_nongen_null_call.cb` (read-only review evidence):

```cflat
import "test_helper.cb";
interface NgLive { int Get(); };
class NgImpl : NgLive { int d = default; int Get() { return d; } };
extern int main() { NgLive lv = default; printf("ctl02 %d\n", (int)lv.Get()); return 0; }
```

Compiles clean and **SIGSEGVs at runtime (exit 139) on both the pre-fix and post-fix binaries.**
`NgLive lv = default;` zero-initialises the `{vtable, data}` fat pointer, and the method call
loads the vtable slot from a null pointer with no guard.

## Also reachable through a generic interface after the routing fix

`scratch/rev4/p/ctl01_null_iface_call.cb` is the same program with `interface Live<T>` /
`Live<int> lv = default;`. It exits 1 on the pre-fix binary and 139 on the post-fix one - **not a
new hazard**: pre-fix, `Live<int>` could not be used as a local at all (that was the bug being
fixed), so the crash was unreachable. Once generic interfaces work, they inherit exactly the
non-generic behaviour above. Fixing this issue fixes both.

## Fix direction

Two candidates, not exclusive:

1. **Reject at compile time** where provable: `IFace lv = default;` with no subsequent assignment
   before a method call is a definite null dispatch. The straight-line tracking already used for
   use-after-move (`RunMoveDataflow`) is the natural home; a definitely-null interface local is the
   same shape of fact.
2. **Guard at run time**: emit a null-vtable check before an interface dispatch and abort with a
   diagnostic (`printf` + `abort`, like the other runtime traps), at least under a debug build.

Option 1 alone leaves the conditional cases; option 2 alone costs a branch per dispatch. A compile
-time rejection for the provable case plus a runtime guard behind the existing debug switch is
probably the right split.

Note `IFace lv = default;` must stay legal to DECLARE - `Test/test_interface.cb` and the
`struct H { IFace c = default; }` field shape both rely on a default-initialised interface slot
that is assigned later. Only the CALL on a still-null value is the error.

## Repro files

`scratch/rev4/p/ctl02_nongen_null_call.cb` (non-generic, the canonical form),
`scratch/rev4/p/ctl01_null_iface_call.cb` (generic).
