# Assigning into a `unique <interface>` local declared WITHOUT an initializer segfaults

Filed 2026-08-09 by `fix/uqinit` while fixing
`assign-into-an-uninitialized-unique-pointer-local-drops-the-rest-of-the-function.md`. It is the
FAT-INTERFACE-VALUE sibling of that defect and was deliberately left out of that fix - see
"Why it was not fixed with the pointer spelling" below.

Severity: **SIGSEGV** (`--no-opt`: rc 139). The default optimized build folds the undef load and
silently skips every statement after the assignment.

## Repro

```cflat
interface IThing { int get(); };
class Res : IThing { int id = 0; int get() { return id; } };
extern int main()
{
    unique IThing p;            // declared with NO initializer
    printf("a\n");
    p = new Res();              // everything below this is dropped
    printf("b %d\n", p.get());
    return p.get() + 42;
}
```

Measured on the merge base `2f5a91a` AND on the landed pointer fix (both spellings identical):

- default opt: prints only `a`, rc 0 (the computed answer is 42)
- `--no-opt`: prints only `a`, rc 139

The `= nullptr` control is not available for this spelling. The nearest one, `unique IThing p =
default;`, is CORRECT on the same binaries - rc 42, prints `a` then `b 0` at both opt levels - so
here too the initializer is the whole difference.

## Root cause

The same one the pointer issue had: the declaration allocates the fat-value slot and never
initializes it, so the assignment's drop-old reads a garbage `{ obj, vtable }` pair and dispatches
the release through the garbage vtable.

## Why it was not fixed with the pointer spelling

The landed fix (`MainListener_Declarations.cpp`, in the local declarator arm) zeroes the slot only
when `typeAndValue.Pointer` is set, which is exactly the `unique T*` / `unique T*[N]` family. A fat
interface value is NOT `Pointer`, and zeroing it would add a store the LANDED
`ReportNullIfaceUninitAccess` check reads as an initialization: that check's discriminator is "this
(Base, empty Path) location has ZERO stores anywhere in F". Measured on the merge base:

```cflat
int f() { unique IThing uv; return uv.get(); }
```

-> `method call on uninitialized interface value 'uv' - ...` (rc 1). A zero-init would silence that
wording. It may well be replaced by `ReportNullIfaceAccess`'s "last set to null" wording, which is
still a rejection and arguably better - but that is a diagnostic-behaviour decision on a landed
feature, not a ride-along on a pointer fix.

## Fix direction

Two candidate answers, and the choice is the work:

1. Zero the fat slot at the declaration too, and re-point the never-initialised check at a
   declaration witness rather than at "no stores in F", so the diagnostic survives. Both spellings
   in `Test/errors/err_iface_field_missing.cb` (legs `uvNeverInitCall`, and the field twin) must
   keep their exact wording; add the `unique` spelling as its own leg either way.
2. Reject `unique <interface> p;` with no initializer outright, naming `= default` as the remedy.
   Cheaper, but it removes an idiom every other local kind supports.

Whichever is taken, add a value leg beside `testUninitializedUniqueLocalDecl()` in
`Test/test_move.cb` asserting a value the dropped statements compute plus a destructor count.
