# Consuming a field of a BY-VALUE struct parameter double-frees the caller's original

Filed 2026-08-09 by the work on `fix/parmbrace`. Not a regression of it: measured identical on
`3252c01` (the branch point) and on `fix/parmbrace`, and all three store spellings behave the same,
so it is one pre-existing gap, not a brace-path asymmetry.

Severity: double free (abort, rc 134).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
struct Wrap { UBox b; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

int f(Wrap w) { UBox[2] dst = { w.b }; return dst[0].item->id; }   // rc 134
int g(Wrap w) { UBox[2] dst; dst[0] = w.b; return dst[0].item->id; } // rc 134
int h(Wrap w) { UBox o = w.b; return o.item->id; }                   // rc 134

extern int main() { { Wrap w; w.b = umk(3); printf("v=%d\n", f(w)); } return 0; }
```

Measured (`scratch/pb_61_fieldsrc.cb`, `pb_62_fieldsrc_assign.cb`, `pb_63_fieldsrc_declinit.cb`):
every spelling prints `v=3`, frees the resource once in the callee, then aborts on the caller's
`w` teardown - rc 134 on both binaries.

## Root cause

`w` is a BORROWED by-value struct parameter: the callee got a bit copy, and the caller still owns
the original `w.b`. The consuming-store arms treat `w.b` as an ordinary INDIRECT owning lvalue,
consume it, and null only the CALLEE's copy of the field. Nothing tells the caller, and the
caller's `Wrap` destructor frees the same `Res` the callee's element already freed.

The owning-sink inference cannot see this either: `CollectConsumedStoreNames` records the source
text `w.b`, which never equals a parameter name, so `w` is not made a sink. That is deliberate -
only a whole-value consume of the parameter counts - but it leaves this shape unguarded.

`ParseMoveExpression` ALREADY rejects the explicit spelling of exactly this with a good message
("cannot 'move' field 'Wrap.b' out of borrowed by-value parameter 'w' ... Declare the parameter
'move w'"). The implicit stores do not consult that guard.

## Fix direction

Route the implicit consuming stores through the same borrowed-by-value-parameter test
`ParseMoveExpression` uses (`IsBorrowedStructParameter` on the source's `ParentVariableName`) and
emit the same diagnostic, so `dst[0] = w.b` / `UBox[2] dst = { w.b }` / `UBox o = w.b` reject
exactly as `move w.b` already does. Do NOT narrow the consume arms back to a bit copy: the bit copy
is what double-freed in the first place.

## Related

Sibling of `internal/issue/p2/implicit-consume-of-a-borrowed-parameters-field-has-no-diagnostic.md`,
which is the POINTER-parameter (`Wrap* w`) spelling. That one is memory-SAFE (rc 0) and only loses
the caller's value silently; this by-VALUE spelling aborts, so it is a level worse and wants the
same single ruling. A pointer-parameter leg is pinned green in
`Test/test_move.cb` (`bps_ptrfieldsrc_*`).
