# Residue of the field-to-field `unique` copy fix: temp/container-element and fat-interface sources

Residue of `unique-field-to-field-copy-double-frees`, closed by the commit that widened
`IsUniqueFieldRead` (`cflat/MainListener.h`) onto generic substitution, reusing the same
owning-slot predicate (`IsOwningUniquePointerField`) already used for the destination.

That commit closes far more than its headline repro. Confirmed rejected post-fix, silently
double-freeing pre-fix, all with a NAMED-LOCAL-shaped source read (the `IsUniqueFieldRead`
GEP-shape test matches): a plain local (`c.t = a.t`, the headline repro), a pointer-to-struct
source (`c.t = bp->t`), a by-value `Box<>` parameter, a fixed-array element, a nested field, a
generic CLASS (not just a generic struct), a bare self-field read inside the owner's own method
(`other.t = t`), a type-alias spelling, a chained assignment, a global source, and a `move`
PARAMETER's field read (`other.t` where only `other`, not the field, was moved - a real,
previously-silent double-free closed as a side effect of this gate change; `move other.t` is the
escape hatch).

Two source shapes remain undiagnosed with the exact same silent double-free, for two DIFFERENT
reasons. Not a regression: both shapes double-freed identically before this commit too.

Severity: silent abort (exit 134 in these repros), no diagnostic at all - same class as the
closed issue.

**Stability caveat**: the double-free in every repro below is a use-after-free read. The exact
stdout value and exit code are NOT a stable oracle - the same source has been observed to print
a different (garbage) value and abort with a different code (e.g. `temp 4`, exit 133) under a
different build/output path. Treat "compiles, then aborts with no diagnostic" as the signature
to match, not the specific number.

## Repro 1 - temp / call-result source (root cause: fails the GEP-shape test)

```cflat
struct Item { int v = default; };
struct Box<T> { T t = default; };
Box<unique Item*> makeBox() { Box<unique Item*> b = default; b.t = new Item(); b.t->v = 70; return b; }
extern int main() { Box<unique Item*> c = default; c.t = makeBox().t; printf("temp %d\n", c.t->v); return 0; }
```

Measured on the pre-fix binary (`3b6e3e8`) and on the fix commit, both identical:
```
temp 70
```
exit 134 (compile rc 0, run rc 134) on both.

The PLAIN equivalent IS diagnosed on both binaries (no change from the fix, since the source
was already written-`unique` and the destination gate was widened by the prior borrowed-param
round):
```cflat
struct Holder { unique Item* slot = nullptr; };
Holder makeH() { Holder h = default; h.slot = new Item(); h.slot->v = 70; return h; }
extern int main() { Holder c = default; c.slot = makeH().slot; printf("ptemp %d\n", c.slot->v); return 0; }
```
```
uf2f_plain_temp_source.cb(4,40): cannot store unique field 'makeH.slot' into unique field
'Holder.slot' - the source field's synthesized destructor already frees it, and two 'unique'
fields cannot own one pointer. Use 'move makeH.slot' to transfer ownership out of the source
field (which nulls it).
```
compile rc 1 on both binaries.

**Why the gate misses it**: `IsUniqueFieldRead` requires `nv.Storage` to be a
`GetElementPtrInst` with exactly 2 indices into a struct type (a field GEP off a named,
alloca-backed local). A field read off a call-result temp (`makeBox().t`) does not carry that
shape - the temp is not addressed the same way a named local is - so the `gep` check at the end
of `IsUniqueFieldRead` fails and the function returns false regardless of the ownership gate.

### Repro 1b - container-element source (SAME root cause, the realistic spelling)

A real program is far more likely to hit this through a container than a bare function call:

```cflat
import "list.cb";
struct Item { int v = default; };
struct Box<T> { T t = default; };
extern int main()
{
    list<Box<unique Item*>> l;
    Box<unique Item*> b = default;
    b.t = new Item();
    b.t->v = 55;
    l.add(move b);
    Box<unique Item*> c = default;
    c.t = l.get(0).t;
    printf("elem %d\n", c.t->v);
    return 0;
}
```

Measured on both binaries, identical:
```
elem 55
```
exit 134 (compile rc 0, run rc 134) on both. `list<T>::get(int)` returns `alias T` (a borrow,
see `cflat/core/list.cb`), and that borrowed return's `.t` field read does not land on a 2-index
struct GEP off a named local either - same `IsUniqueFieldRead` shape miss as repro 1, not a
distinct root cause.

## Repro 2 - fat-interface source (root cause: TWO independent blockers, different from repro 1)

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };
struct Box<T> { T t = default; };
extern int main()
{
    Box<unique IShape> a = default;
    a.t = new Circle();
    Box<unique IShape> c = default;
    c.t = a.t;
    printf("iface %d\n", c.t.area());
    return 0;
}
```

Measured on the pre-fix binary and on the fix commit, both identical:
```
iface 0
```
exit 134 (compile rc 0, run rc 134) on both.

The PLAIN equivalent (`unique IShape v` field, not generic) IS diagnosed on both binaries -
unchanged by this fix. Note the setup must use a legal owning store (`a.v = new Circle();`) or
compilation never reaches the field-to-field copy at all - an earlier repro in this file's first
draft mistakenly quoted the SETUP line's diagnostic as if it were about the copy; corrected here:

```cflat
struct Holder { unique IShape v = default; };
extern int main()
{
    Holder a = default;
    a.v = new Circle();
    Holder c = default;
    c.v = a.v;
    printf("piface %d\n", c.v.area());
    return 0;
}
```
```
uf2f_plain_iface_source2.cb(9,4): cannot assign a borrowed value to unique interface 'c' - the
source still owns it (or is a stack value), so this would leak or free a stack address at scope
exit; assign 'new', a 'move' expression, a move-returning call, or 'nullptr'
```
compile rc 1 on both binaries, at line 9 (`c.v = a.v;`) - the field-to-field copy itself, via a
DIFFERENT pre-existing check (assign-borrowed-value-into-unique-interface), not the
field-to-field two-owners message this fix's leg produces.

**Why the gate misses it - TWO independent blockers, either one alone would suffice to block
it**:
1. `IsOwningUniquePointerField`'s generic (`IsUniqueTypeArg`) arm requires `tv.Pointer`. A fat
   interface value substituted for `T` is a two-word vtable+data struct, not a raw pointer, so
   `tv.Pointer` is false and the ownership gate itself returns false.
2. Independently, the `=` call site's own guard (`cflat/MainListener.h:12264`,
   `right->getType()->isPointerTy()`) is ALSO false for a fat interface LLVM value
   (`rightPtrTy=0 rightStructTy=1`). Widening only `IsOwningUniquePointerField` and leaving this
   guard alone would still let the leg silently fail to fire.

## Related issues

- [[interface-field-self-assign-false-positive]] - a DIFFERENT defect found while probing this
  area: an interface-field store IS diagnosed once the ownership gate matches, but a false
  self-assign read (both receivers carry an empty `CallerName`) can suppress the diagnostic
  entirely on a scalar interface-field copy. Pre-existing, not caused by this fix.
- [[generic-unique-field-temp-source-crashes-compiler]] - shares the temp/call-result source
  spelling with repro 1 above, but is a DIFFERENT failure: a user-written destructor on the
  pointee turns the silent runtime double-free into a compile-time SIGSEGV instead.

## Fix direction

Not scoped here. Closing repro 1 (and 1b) needs a provenance signal for "this borrowed
temporary's only root is a just-returned or just-borrowed owning field" (the temp/borrow itself
has no Storage to test, so the shape test needs to move earlier, to the call-result
materialization site, or track ownership through the temp). Closing repro 2 needs the
field-to-field checks to recognize a fat-interface owning slot the way the pre-existing
"borrowed value into unique interface" check already does for the WRITTEN `unique IShape`
spelling, then apply the same field-to-field two-owners reasoning to the generic substitution of
that shape - touching both the ownership-gate `tv.Pointer` requirement and the `=` call site's
own `isPointerTy()` guard. All three are real, separately scoped follow-ups; do not fold them
into a reflexive widening of `IsUniqueFieldRead`'s GEP-shape test, which risks over-matching
borrows through casts (see the existing `IsUniqueFieldAlias` carve-out in that function).
