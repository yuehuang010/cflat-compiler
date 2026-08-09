# A `T[]` VIEW field's brace default is discarded, and reading the view segfaults

Filed 2026-08-09 by the review of `fix/fldarr`. PRE-EXISTING: every measurement below is
identical on `987ae77` and on `d8056c1`. `fix/fldarr` deliberately left the view spelling
alone; this file records what that shape actually does, because the reasoning given for
skipping it ("a view owns no storage, so there is nothing for the list to write into") is not
what the local declarator does.

Severity: bad code gen - a silently discarded initializer that leaves a null view, so the
first read segfaults.

## Repro

```cflat
struct H { int[] v = { 1, 2, 3 }; };
extern int main() { H h; printf("%d\n", h.v[0]); return 0; }
```

-> compiles rc 0 with no diagnostic, runs, **rc 139** (the view is null).
Constructing without reading (`H h; printf("constructed\n");`) exits 0, so the fault is the
deref, not the construction.

Oracle, verified independently: the LOCAL spelling works.
`int[] v = { 1, 2, 3 };` inside `main` prints `1`, rc 0.

## Root cause

`MainListener::ParseFieldDefaultBraceInitializer` returns `nullptr` for `field.IsArrayView`,
so the caller's no-initializer handling zero-fills the `{ptr, len}` slot.

The premise that a view has nothing to write into is false at the declarator: `T[] x = {...}`
is the LENGTH-INFERRED form, and `MainListener::EmitArrayViewInferredInit` allocates the
backing storage from the list and points the view at it. A field would need the same storage,
with a lifetime tied to the containing object - which is the real reason this is hard, and is
not the reason recorded in `d8056c1`.

## Fix direction

Two defensible answers; pick one and pin it:

1. REJECT the spelling in the field position, the way the other non-buildable field brace
   shapes are rejected (`LogNonAggregateBraceInitReject` / `LogPointerBraceInitReject`), with
   a message naming the storage-lifetime reason and pointing at `T[N]`.
2. BUILD it by giving the containing object a hidden `[N x T]` and aiming the view at it -
   substantially more work, and it changes the object's layout.

Option 1 is the cheap correct step: today the program compiles and then dies with no
diagnostic at all.
