# A '??' join of class pointers assigned to an interface is never boxed

Filed 2026-07-29 while fixing `global-primitive-array-boxed-into-interface`. PRE-EXISTING
and unrelated to that fix: identical on `df32dd8` and on the fix commit.

Severity: hard compile failure with NO source diagnostic - only an LLVM module-verifier
dump naming an unnamed value. A legal-looking program cannot be compiled, so this is a
FALSE REJECTION in effect, and the user is given nothing to act on.

## Not covered by the two queue entries it resembles

- [[interface-boxing-guards-are-binding-dependent]] is about parens / `?:` erasing the
  binding the OWNERSHIP guards key off, and its symptom is a double free (exit 134).
- [[return-ternary-join-concrete-pointers-not-boxed]] is the RETURN path.

A `??` join in DECL-INIT is a third shape: the boxing never happens at all, and it fails at
the verifier rather than at runtime.

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = default; int area() { return r * r; } };

extern int main()
{
    Circle* maybe = nullptr;
    Circle* p = new Circle(); p->r = 9;
    IShape s = maybe ?? p;       // no diagnostic; module verification fails
    printf("%d\n", s.area());
    return 0;
}
```

Both binaries, `-o` and `--check`, exit 1 with:

```
Module verification failed:
Invalid bitcast
  %10 = bitcast ptr %9 to %__iface_fat_ptr
```

`IShape s = p;` and the `?:` spelling `IShape s = c ? p : q;` both work, so the trigger is
`??` specifically.

## Root cause

Not diagnosed. The shape is the familiar one: the decl-init interface path needs a
`TypeName` on the RHS NamedVariable to pick a boxing branch, and the `??` join result
carries none. The `?:` join has a dedicated recovery (`UpcastTernaryPhiToInterface`, reached
when `structName` is empty); `??` produces a select/phi that either is not a `PHINode` or is
not routed to that helper, so every branch of the chain is skipped and the raw `ptr` is
stored into the fat slot. That last sentence is the SAME fall-through mechanism as the
primitive-array bug just fixed - only the reason the TypeName is missing differs.

## Fix direction

Route the `??` join through the same per-arm boxing the `?:` join already uses, or - if the
join's arms cannot be recovered - reject it with the wording `?:` uses when an arm cannot be
resolved ("bind the arm to a local variable of the class type first"). Either way it must
not reach the module verifier.
