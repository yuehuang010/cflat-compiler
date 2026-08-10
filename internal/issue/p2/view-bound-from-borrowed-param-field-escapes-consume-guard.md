# A `T[]` view bound from a borrowed by-value param's array field escapes the consume guard

Filed 2026-08-10 by the review of `fix/spanown` (probe `rev_c3`), measured IDENTICAL on `6a8e7a9`
and on `fix/spanown` - pre-existing, not a regression of the view-element or return-arm work.

Severity: double free (abort, rc 133), no diagnostic.

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
struct Holder { UBox[2] arr; };

UBox f(Holder h) { UBox[] v = h.arr; return v[0]; }   // rc 133, guard never fires
// control: UBox g(Holder h) { return h.arr[0]; }     // rejected: cannot consume field ... 'h'
```

The decl-init spelling `UBox q = v[0];` in the same shape is rc 133 on both binaries too, so the
two consume positions agree - the hole is upstream of both.

## Root cause (direction)

`RejectConsumeOfBorrowedByValueParamField` keys on the source's field-path provenance
(`OwningStructName` / `FieldPathRoot`). Binding `UBox[] v = h.arr;` launders the borrowed
parameter's storage through the view binding: the element NamedVariable carries `IsViewElement`
(so the consume arms fire) but not the root-of-borrowed-param fact (so the guard stays blind).
Through a POINTER receiver (`Holder* h`) the same shape consumes correctly (rc 0), so the missing
fact is specifically the by-value-param root.

## Fix direction

Carry the view base binding's root provenance (at minimum RootIsBorrowedByValueParam) onto the
element NamedVariable the same way `IsViewElement` is carried, then the existing guard fires at
the existing consume arms - no new arm needed. Same family as
[[parenthesized-operand-loses-named-variable-provenance]] (provenance dropped at a binding
boundary); if the NamedVariable-threading fix lands there, re-measure this shape before starting.
