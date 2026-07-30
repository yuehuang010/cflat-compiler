# A namespace-local interface shadowed by a same-named GLOBAL interface cannot be used at all

Filed 2026-07-30, found by the adversarial review of
[[interface-issue-queue]] (landed design records) (round 2). **Pre-existing on both binaries** and
independent of generics - the type-argument fix only newly ROUTES working programs into it.

Severity: **false rejection with a nonsense diagnostic.** No wrong value.

## Repro

Two interfaces of the same name, one global and one in a namespace. Assigning a namespace-local
implementor to the namespace-local interface fails.

`scratch/rev5/s3g_control_localvar.cb` - non-generic, bare spelling:

```cflat
interface IThing { int Val(); };
namespace A
{
    interface IThing { int Val(); };
    class AThing : IThing { int Val() { return 7; } };
    int f() { AThing a = default; IThing h = a; return h.Val(); }
}
```

```
s3g_control_localvar.cb(8,45): 'A.AThing' does not implement interface 'A.IThing'
```

`A.AThing`'s base clause names exactly `A.IThing` and implements its only method, so the diagnostic
is simply false.

`scratch/rev5/s3h_control_field.cb` - non-generic, QUALIFIED spelling, as a struct field:

```
s3h_control_field.cb(10,55): cannot cast an aggregate value - a fixed array decays to a pointer to
its first element
```

**That message is nonsense for this program** - there is no array anywhere in the file. It is the
aggregate-cast diagnostic reused for an interface assignment whose fat-pointer conversion was never
built. Any user who hits this is sent looking in entirely the wrong place; worth fixing even before
the underlying bug.

Both fail identically on `15809e0` and on the type-argument fix, in Release.

## Scope, established by bisection

| Probe | Shape | PRE | fixed binary |
|---|---|---|---|
| `s3b_iface_min.cb` | ONLY `A.IThing` exists, bare argument | `unknown type 'IThing'` | works (7) |
| `s3c_iface_qualified.cb` | ONLY `A.IThing`, qualified argument | works (7) | works (7) |
| `s3d_two_ifaces_Aleg.cb` | global + `A.IThing`, bare argument, A leg used | works (7) | `cannot cast an aggregate value ...` |
| `s3e_two_ifaces_globalleg.cb` | global + `A.IThing`, GLOBAL leg used | works (9) | works (9) |
| `s3f_qualified_inA.cb` | global + `A.IThing`, QUALIFIED argument | fails | fails |
| `s3g_control_localvar.cb` | non-generic control, bare local | fails | fails |
| `s3h_control_field.cb` | non-generic control, qualified field | fails | fails |

The discriminator is **the presence of a same-named global interface**, not generics and not the
spelling: `s3c` (no global) works on both, `s3f` (global present) fails on both. `s3d` only appeared
to work on PRE because the bare argument never resolved to `A.IThing` at all - it silently named the
GLOBAL interface, which happened to have an identical contract. So PRE's `7` was the collapsed-name
bug returning a coincidentally-right answer, not working shadowing.

## Root cause direction

Not diagnosed. The two candidates, in order:

- The struct-wins / interface-routing tie-break (`IsGenericInterfaceTemplateName`,
  `RevokeGenericInterfaceInstances`) and `ResolveInterfaceName` resolve an interface NAME through
  several different key conventions; a same-named global is exactly the case where the bare tail and
  the qualified key disagree. Compare
  [[bare-interface-name-resolves-outward-before-namespace]], which is the resolution-ORDER half of
  the same problem for non-generic interface names.
- The vtable/rebox emission may be keyed on the bare name, so the global interface's slot is found
  where the namespace-local one's was wanted - which would explain an aggregate-cast failure rather
  than a lookup failure.

Fix the diagnostic either way: an interface assignment must never report a fixed-array decay.

## Consequence for the type-argument fix

`generic-type-arguments-not-key-space-resolved` resolves a bare type ARGUMENT through the
enclosing-namespace chain. That turns a bare `Box<IThing>` written inside `namespace A` into
`Box<A.IThing>`, which is the correct meaning - and lands it on this pre-existing defect whenever a
same-named global interface exists. The routing change is ratified (the qualified spelling and both
non-generic controls already fail identically on both binaries, so the fixed binary has converged
onto the compiler's own answer for the shape), and the programs it newly rejects were only working
by accident of the collapsed name. This issue owns the actual repair.

Related: [[interface-issue-queue]] (landed design records),
[[bare-interface-name-resolves-outward-before-namespace]], [[interface-issue-queue]]
