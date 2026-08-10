# Null-interface proof false-rejects a field initialized to a heap implementation

## Summary

A struct field of interface type initialized to a fresh heap implementation in the
field default is still reported as "has not been assigned an implementation" when a
method is called through it. False rejection of valid code.

## Repro

```cflat
// PTagged is an interface; PTag implements it
struct B { PTagged c = new PTag(); }

extern int main()
{
    B b = default;
    int k = b.c.kind();   // error: '...' has not been assigned an implementation
    return 0;
}
```

Reproduces at scalar scope with no array involved. Measured identical on master
2f72f43 (pre-existing; surfaced during the arrdef review round 2026-08-09, probe was
scratch/rad_ax19.cb).

## Root cause (hypothesis, unverified)

The null-interface-use proof that guards interface method dispatch does not treat a
field default of the form `= new Impl()` as an assignment of an implementation - it
likely only tracks direct assignments to the variable/field in the current function,
not initialization performed by the (synthesized) default constructor.

## Fix direction

Teach the proof that a field whose default initializer constructs a concrete
implementation is initialized after default construction of the owner. Related
frozen asymmetry: the bare `PTagBox[2] pb;` and `= {}` spellings do not diagnose a
MISSING implementation either way (see err_iface_field_missing.cb legs) - keep that
asymmetry unchanged unless deliberately revisiting it.
